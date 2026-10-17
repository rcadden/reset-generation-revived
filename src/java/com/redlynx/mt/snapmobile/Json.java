package com.redlynx.mt.snapmobile;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Minimal JSON reader/writer for the relay's small control messages. */
final class Json {
    private final String s;
    private int i;

    private Json(String s) {
        this.s = s;
    }

    /** Parses a JSON object; null if the text is not one. Numbers become Double. */
    @SuppressWarnings("unchecked")
    static Map<String, Object> parseObject(String text) {
        try {
            Json p = new Json(text);
            Object v = p.value();
            p.ws();
            return v instanceof Map && p.i == text.length() ? (Map<String, Object>) v : null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    static String quote(String v) {
        StringBuilder b = new StringBuilder("\"");
        for (int k = 0; k < v.length(); k++) {
            char c = v.charAt(k);
            if (c == '"' || c == '\\') {
                b.append('\\').append(c);
            } else if (c < 0x20) {
                b.append(String.format("\\u%04x", (int) c));
            } else {
                b.append(c);
            }
        }
        return b.append('"').toString();
    }

    private void ws() {
        while (this.i < this.s.length() && Character.isWhitespace(this.s.charAt(this.i))) {
            this.i++;
        }
    }

    private char peek() {
        this.ws();
        return this.s.charAt(this.i);
    }

    private void expect(char c) {
        if (this.peek() != c) {
            throw new IllegalArgumentException("expected " + c);
        }
        this.i++;
    }

    private Object value() {
        char c = this.peek();
        if (c == '{') {
            Map<String, Object> m = new HashMap<String, Object>();
            this.i++;
            if (this.peek() == '}') {
                this.i++;
                return m;
            }
            do {
                String k = this.string();
                this.expect(':');
                m.put(k, this.value());
            } while (this.comma('}'));
            return m;
        }
        if (c == '[') {
            List<Object> l = new ArrayList<Object>();
            this.i++;
            if (this.peek() == ']') {
                this.i++;
                return l;
            }
            do {
                l.add(this.value());
            } while (this.comma(']'));
            return l;
        }
        if (c == '"') {
            return this.string();
        }
        if (this.s.startsWith("true", this.i)) {
            this.i += 4;
            return Boolean.TRUE;
        }
        if (this.s.startsWith("false", this.i)) {
            this.i += 5;
            return Boolean.FALSE;
        }
        if (this.s.startsWith("null", this.i)) {
            this.i += 4;
            return null;
        }
        int start = this.i;
        while (this.i < this.s.length() && "+-0123456789.eE".indexOf(this.s.charAt(this.i)) >= 0) {
            this.i++;
        }
        return Double.valueOf(this.s.substring(start, this.i));
    }

    private boolean comma(char end) {
        char c = this.peek();
        this.i++;
        if (c == ',') {
            return true;
        }
        if (c == end) {
            return false;
        }
        throw new IllegalArgumentException("expected , or " + end);
    }

    private String string() {
        this.expect('"');
        StringBuilder b = new StringBuilder();
        while (true) {
            char c = this.s.charAt(this.i++);
            if (c == '"') {
                return b.toString();
            }
            if (c == '\\') {
                char e = this.s.charAt(this.i++);
                switch (e) {
                    case 'n': b.append('\n'); break;
                    case 't': b.append('\t'); break;
                    case 'r': b.append('\r'); break;
                    case 'b': b.append('\b'); break;
                    case 'f': b.append('\f'); break;
                    case 'u':
                        b.append((char) Integer.parseInt(this.s.substring(this.i, this.i + 4), 16));
                        this.i += 4;
                        break;
                    default: b.append(e);
                }
            } else {
                b.append(c);
            }
        }
    }
}
