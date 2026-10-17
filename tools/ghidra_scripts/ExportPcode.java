// Export raw P-code for every instruction of the program, for the static recompiler
// (tools/recomp/). Arg: output path.
//
// Format (one record per line, hex without 0x):
//   F <entry> <name>                       function entry known to Ghidra
//   I <addr> <len> <bytes> <disassembly>   instruction
//   P <opcode> <out|-> <in> <in> ...       its P-code ops, in order
//   J <target>                             computed-jump / indirect-flow target of the last I
// Varnodes are space:offset:size with space r(egister) u(nique) c(onst) m(emory).
// CALLOTHER's first input is written as its user-op name (o:<name>).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Language;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.FlowType;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.mem.Memory;
import java.io.*;

public class ExportPcode extends GhidraScript {
    private Language lang;

    private String vn(Varnode v) {
        if (v == null) return "-";
        String sp;
        if (v.isRegister()) sp = "r";
        else if (v.isUnique()) sp = "u";
        else if (v.isConstant()) sp = "c";
        else if (v.isAddress()) sp = "m";
        else sp = v.getAddress().getAddressSpace().getName();
        return sp + ":" + Long.toHexString(v.getOffset()) + ":" + v.getSize();
    }

    /** MSVC places functions after int3/nop padding or right after a RET; a stray data value
     *  pointing into the middle of real code must not be disassembled (it would block the
     *  real instruction stream). */
    private boolean looksLikeFunctionStart(Memory mem, Address t) {
        try {
            int p1 = mem.getByte(t.subtract(1)) & 0xff;
            if (p1 == 0xcc || p1 == 0x90 || p1 == 0xc3) return true;
            if ((mem.getByte(t.subtract(3)) & 0xff) == 0xc2) return true;
            // right after an instruction that never falls through (JMP, RET): SEH handlers etc.
            Instruction prev = currentProgram.getListing().getInstructionContaining(t.subtract(1));
            return prev != null && prev.getMaxAddress().equals(t.subtract(1)) && !prev.getFlowType().hasFallthrough();
        } catch (Exception e) {
            return false;
        }
    }

    /** Disassembles a plausible function start. If auto-analysis decoded junk that overlaps the
     *  real instruction stream (bytes in the middle of an instruction), that junk is cleared up
     *  to the next int3 padding first. */
    private boolean disassembleAt(Memory mem, Listing listing, Address t) throws Exception {
        Instruction junk = null;
        Address end = t;
        for (int i = 0; i < 0x4000; i++) {
            Address a = t.add(i);
            if ((mem.getByte(a) & 0xff) == 0xcc && (mem.getByte(a.add(1)) & 0xff) == 0xcc) break;
            end = a;
        }
        if (listing.getInstructionContaining(t) != null && listing.getInstructionAt(t) == null) return false;
        boolean existed = listing.getInstructionAt(t) != null;
        if (!existed) {
            DisassembleCommand dc = new DisassembleCommand(t, null, true);
            dc.applyTo(currentProgram, monitor);
            if (listing.getInstructionAt(t) == null) return false;
        }
        // stream stopped by a conflict inside [t, end]? clear the region and redo
        Instruction ins = listing.getInstructionAt(t);
        boolean conflict = false;
        while (ins != null && ins.getMaxAddress().compareTo(end) < 0) {
            if (!ins.getFlowType().hasFallthrough()) break;
            Address next = ins.getMaxAddress().add(1);
            Instruction n = listing.getInstructionAt(next);
            if (n == null) { conflict = true; break; }   // stream stopped early: blocked by overlapping junk
            ins = n;
        }
        if (conflict) {
            clearListing(t, end);
            new DisassembleCommand(t, null, true).applyTo(currentProgram, monitor);
        } else if (existed) {
            return false;
        }
        new CreateFunctionCmd(t).applyTo(currentProgram, monitor);
        return listing.getInstructionAt(t) != null;
    }

    /** Code reachable only through pointers in data (vtables, callback tables) is often left
     *  undisassembled by auto-analysis: disassemble it and make it a function. Repeats until
     *  nothing new is found (new code can reference more data). */
    private void discoverDataPointers() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();
        for (int pass = 0; pass < 10; pass++) {
            int found = 0;
            for (MemoryBlock b : mem.getBlocks()) {
                if (b.isExecute() || !b.isInitialized()) continue;
                Address a = b.getStart();
                while (a.compareTo(b.getEnd().subtract(3)) <= 0) {
                    long v = mem.getInt(a) & 0xffffffffL;
                    Address t = a.getNewAddress(v);
                    MemoryBlock tb = mem.getBlock(t);
                    if (tb != null && tb.isExecute()
                            && listing.getDefinedDataContaining(t) == null && looksLikeFunctionStart(mem, t)) {
                        if (disassembleAt(mem, listing, t)) found++;
                    }
                    a = a.add(4);
                }
            }
            // code addresses used as MOV/PUSH immediates (callbacks stored at run time)
            java.util.List<Address> imms = new java.util.ArrayList<>();
            for (Instruction ins : listing.getInstructions(true)) {
                String m = ins.getMnemonicString();
                if (!m.equals("MOV") && !m.equals("PUSH")) continue;
                for (int i = 0; i < ins.getNumOperands(); i++) {
                    for (Object o : ins.getOpObjects(i)) {
                        if (o instanceof ghidra.program.model.scalar.Scalar) {
                            long v = ((ghidra.program.model.scalar.Scalar) o).getUnsignedValue();
                            Address t = ins.getAddress().getNewAddress(v & 0xffffffffL);
                            MemoryBlock tb = mem.getBlock(t);
                            if (tb != null && tb.isExecute() && listing.getInstructionAt(t) == null
                                    && listing.getDefinedDataContaining(t) == null && looksLikeFunctionStart(mem, t)) imms.add(t);
                        }
                    }
                }
            }
            for (Address t : imms) {
                if (listing.getInstructionAt(t) != null) continue;
                if (disassembleAt(mem, listing, t)) found++;
            }
            println("ExportPcode: data-pointer pass " + pass + ": " + found + " new code entries");
            if (found == 0) break;
        }
    }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        lang = currentProgram.getLanguage();
        discoverDataPointers();
        Listing listing = currentProgram.getListing();
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out), 1 << 20))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                w.println("F " + Long.toHexString(f.getEntryPoint().getOffset()) + " " + f.getName());
            }
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isExecute()) continue;
                InstructionIterator it = listing.getInstructions(b.getStart(), true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Instruction ins = it.next();
                    if (!b.contains(ins.getAddress())) break;
                    StringBuilder hex = new StringBuilder();
                    for (byte x : ins.getBytes()) hex.append(String.format("%02x", x & 0xff));
                    w.println("I " + Long.toHexString(ins.getAddress().getOffset()) + " " + ins.getLength() + " " + hex + " " + ins.toString());
                    for (PcodeOp op : ins.getPcode()) {
                        StringBuilder s = new StringBuilder("P ").append(op.getMnemonic()).append(' ').append(vn(op.getOutput()));
                        for (int i = 0; i < op.getNumInputs(); i++) {
                            Varnode in = op.getInput(i);
                            if (i == 0 && op.getOpcode() == PcodeOp.CALLOTHER) {
                                s.append(" o:").append(lang.getUserDefinedOpName((int) in.getOffset()));
                            } else {
                                s.append(' ').append(vn(in));
                            }
                        }
                        w.println(s);
                    }
                    FlowType ft = ins.getFlowType();
                    if (ft.isComputed()) {
                        for (Reference r : ins.getReferencesFrom()) {
                            if (r.getReferenceType().isFlow()) {
                                w.println("J " + Long.toHexString(r.getToAddress().getOffset()));
                            }
                        }
                    }
                }
            }
        }
        println("ExportPcode: wrote " + out);
    }
}
