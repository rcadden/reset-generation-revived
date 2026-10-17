package com.redlynx.mt.snapmobile;

/**
 * Reset Generation Revived replacement: friends and presence are not supported by the relay
 * yet, so these are no-ops; "logged in" means connected to the relay.
 */
public class ServerCommBridge {
    private volatile boolean connected;

    void setConnected(boolean connected) {
        this.connected = connected;
    }

    public void acceptBuddyRequest(String username) {
    }

    public void declineBuddyRequest(String username) {
    }

    public void removeBuddy(String username) {
    }

    public void requestBuddy(String username, String message) {
    }

    public void setPresence(int availability, String presence) {
    }

    public boolean isLoggedIn() {
        return this.connected;
    }
}
