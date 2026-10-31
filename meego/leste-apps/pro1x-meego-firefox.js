// Leste's Firefox ESR in the MeeGo session (leste-apps): no offline mode of
// its own. Firefox decides "offline" from the kernel's routes (netlink; there
// is no NetworkManager on Leste) and goes offline whenever icd2 drops the
// Wi-Fi (on 2026-10-31 its tab still showed the error page five minutes after
// the reconnect; whether it was reloaded is not known). With this it simply
// tries the network each time.
// Installed by meego/41-leste-helpers.sh into /etc/firefox-esr, whose files
// Debian's Firefox reads as default preferences (about:config can override).
pref("network.manage-offline-status", false);
