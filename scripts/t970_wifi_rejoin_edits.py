"""SM-T970 Wi-Fi rejoin after a restart (kernel z92), as edits of drivers/PongWifi/PongWifiNet.cpp.

iOS's wifid does not join the remembered home network by itself after a restart (11-14..11-16: it scans and sends only
the 481/483 requests). PongWifiNet keeps the last network the daemon joined (name and passphrase, or the 64 hex digits of
the PMK the daemon derived); bsd/dev/pong_wifi_tools.c saves it on the Data volume (root, 0600) once that join is up and,
after the next boot, asks for the same join when the network is in the scan results and the daemon has not joined or
left anything itself. A kernel join reports exactly what a daemon join reports (on_connect_done: link on, association
done, SSID/BSSID changed), so the daemon and the address configuration follow it. Nothing is logged of the name or key.
"""

REJOIN_EDITS = (
    ('bool gUserJoin;   // the wireless daemon joined, left or switched the radio itself: the unattended test join steps aside\n',
     'bool gUserJoin;   // the wireless daemon joined, left or switched the radio itself: the unattended test join steps aside\n'
     '// SM-T970 rejoin (z92): the last network the daemon asked for, and how often it joined or left since boot\n'
     'uint8_t gLastSsid[32]; unsigned gLastSsidLen, gLastPassLen, gLastSeq, gDaemonJoinOps; char gLastPass[65];\n'),
    ('      memset(pass, 0, sizeof pass); memset(&a, 0, sizeof a);\n      if(!r)pong_wifi_wake();\n',
     '      gDaemonJoinOps++;\n'
     '      if (!r) { memcpy(gLastSsid, a.ssid, a.ssid_length); gLastSsidLen = a.ssid_length; gLastPassLen = hasKey ? keyLength : 0;\n'
     '        memset(gLastPass, 0, sizeof gLastPass); if (hasKey) memcpy(gLastPass, pass, keyLength); gLastSeq++; }\n'
     '      memset(pass, 0, sizeof pass); memset(&a, 0, sizeof a);\n      if(!r)pong_wifi_wake();\n'),
    ('case kDisassociate: case kDeauth: gUserJoin = true;',
     'case kDisassociate: case kDeauth: gUserJoin = true; gDaemonJoinOps++;'),
    # z95: the module's start_connect() begins only from idle or scanning and returns without a word when joined, so a
    # switch to another network (11-16: wi_gen_5G_56 -> wi_gen_2.4G_45) was taken and never done; iOS timed out with
    # "cannot join". Leave the current network first (as kDisassociate does), then ask for the join.
    ('      gKeyed = hasKey; int r = nw_wifi_connect(',
     '      { nw_wifi_status cur; nw_wifi_status_get(&cur);\n'
     '        if (cur.state >= NW_STA_AUTH) {\n'
     '          log(6, "wifi-net: join while joined (state %u): leaving the current network first", cur.state);\n'
     '          nw_wifi_disconnect(); pong_wifi_wake(); notify_locked(NotifyLinkDown);\n'
     '          for (int i = 0; i < 100 && cur.state >= NW_STA_AUTH; i++) { IOSleep(20); nw_wifi_status_get(&cur); }\n'
     '          log(6, "wifi-net: left, state %u", cur.state);\n'
     '        } }\n'
     '      gKeyed = hasKey; int r = nw_wifi_connect('),
)

REJOIN_TAIL = r'''
// SM-T970 rejoin (z92), for bsd/dev/pong_wifi_tools.c. The last join the daemon asked for: its sequence number (0 = none
// since boot), name, key, the daemon's joins and leaves since boot, the station state and whether the link that is up is
// that network.
extern "C" unsigned pong_wifi_last_join(uint8_t *ssid, unsigned *ssid_len, char *pass, unsigned *pass_len, unsigned *daemon_ops,
                                        int *state, int *same);
extern "C" unsigned pong_wifi_last_join(uint8_t *ssid, unsigned *ssid_len, char *pass, unsigned *pass_len, unsigned *daemon_ops,
                                        int *state, int *same) {
  nw_wifi_status st; nw_wifi_status_get(&st);
  lck_mtx_lock(gLock);
  const unsigned seq = gLastSeq;
  memcpy(ssid, gLastSsid, 32); *ssid_len = gLastSsidLen; memcpy(pass, gLastPass, 65); *pass_len = gLastPassLen;
  *daemon_ops = gDaemonJoinOps;
  lck_mtx_unlock(gLock);
  *state = static_cast<int>(st.state);
  *same = st.state == NW_STA_UP && st.ssid_len == *ssid_len && !memcmp(st.ssid, ssid, *ssid_len);
  return seq;
}
// Join a saved network again: only when it is in the scan results, the radio is on, the station is not joining or joined and
// the daemon has not joined or left anything since boot. 0 = asked, ENOENT = not seen, EBUSY = not now.
extern "C" int pong_wifi_rejoin(const uint8_t *ssid, unsigned ssid_len, const char *pass, unsigned pass_len);
extern "C" int pong_wifi_rejoin(const uint8_t *ssid, unsigned ssid_len, const char *pass, unsigned pass_len) {
  if (!ssid_len || ssid_len > 32 || pass_len > 64) return EINVAL;
  int r = ENOENT;
  lck_mtx_lock(gLock);
  nw_bss_info b; const int n = nw_wifi_bss_count(); bool seen = false;
  for (int i = 0; i < n && !seen; i++) seen = !nw_wifi_bss_get(i, &b) && b.ssid_len == ssid_len && !memcmp(b.ssid, ssid, ssid_len);
  if (seen) {
    nw_wifi_status st; nw_wifi_status_get(&st);
    r = EBUSY;
    if (gPower && st.state < NW_STA_AUTH && !gDaemonJoinOps) {
      char p[65]; memset(p, 0, sizeof p); memcpy(p, pass, pass_len);
      gAssocResult = kAssocFailed; gKeyed = pass_len != 0;
      r = nw_wifi_connect(ssid, ssid_len, nullptr, pass_len ? p : nullptr, pass_len) ? EIO : 0;
      memset(p, 0, sizeof p);
    }
  }
  lck_mtx_unlock(gLock);
  if (!r) pong_wifi_wake();
  log(6, "wifi-net: rejoin by the kernel (key length %u): %d", pass_len, r);
  return r;
}
'''
