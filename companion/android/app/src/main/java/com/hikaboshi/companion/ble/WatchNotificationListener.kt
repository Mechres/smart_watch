package com.hikaboshi.companion.ble

import android.app.Notification
import android.app.NotificationManager
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Forwards phone notifications to the watch while the link is up.
 * Enabled from the app's Notify tab (system listener access + in-app toggle).
 * Ongoing notifications, system packages and rapid duplicates are skipped.
 * Also applies the watch's dismiss/DND-toggle button presses, and pauses
 * forwarding while the phone's own Do Not Disturb is active.
 */
class WatchNotificationListener : NotificationListenerService() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var controlJob: Job? = null

    /** System key of the last notification actually forwarded; target of a watch dismiss. */
    private var lastForwardedSbnKey: String? = null

    override fun onListenerConnected() {
        super.onListenerConnected()
        controlJob?.cancel()
        controlJob = scope.launch {
            BleHolder.get(applicationContext).controlEvents.collect { cmd -> handleControl(cmd) }
        }
    }

    override fun onListenerDisconnected() {
        controlJob?.cancel()
        controlJob = null
        super.onListenerDisconnected()
    }

    private fun handleControl(cmd: String) {
        when (cmd) {
            Protocol.EVT_DISMISS_NOTIF -> {
                lastForwardedSbnKey?.let { key ->
                    try {
                        cancelNotification(key)
                    } catch (_: Exception) {
                    }
                }
                lastForwardedSbnKey = null
            }
            Protocol.EVT_DND_TOGGLE -> toggleDnd()
        }
    }

    private fun toggleDnd() {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        if (!nm.isNotificationPolicyAccessGranted) return
        nm.setInterruptionFilter(
            if (nm.currentInterruptionFilter == NotificationManager.INTERRUPTION_FILTER_ALL) {
                NotificationManager.INTERRUPTION_FILTER_PRIORITY
            } else {
                NotificationManager.INTERRUPTION_FILTER_ALL
            },
        )
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        val sbn = sbn ?: return
        val mgr = BleHolder.peek() ?: return
        if (!mgr.forwardingEnabled()) return
        if (!mgr.state.value.connected) return
        if (sbn.isOngoing) return
        if (sbn.packageName in DENYLIST) return
        if (sbn.packageName in BleHolder.disabledNotifApps(this)) return
        // Phone is silencing itself; don't buzz the watch either.
        val nm = getSystemService(NotificationManager::class.java)
        if (nm?.currentInterruptionFilter != NotificationManager.INTERRUPTION_FILTER_ALL) return

        val extras = sbn.notification.extras
        var title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()?.trim().orEmpty()
        var text = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString()?.trim().orEmpty()
        if (text.isEmpty()) {
            text = extras.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()?.trim().orEmpty()
        }
        if (title.isEmpty()) title = appLabel(sbn.packageName)
        if (title.isEmpty() && text.isEmpty()) return

        val now = SystemClock.elapsedRealtime()
        val key = "${sbn.packageName}|$title|$text"
        synchronized(lock) {
            if (key == lastKey && now - lastKeyTime < DEDUP_MS) return
            if (now - lastForward < MIN_GAP_MS) return
            lastKey = key
            lastKeyTime = now
            lastForward = now
        }
        scope.launch {
            try {
                mgr.sendNotification(title, text)
                lastForwardedSbnKey = sbn.key
            } catch (_: Exception) {
                // Link dropped mid-send; the reconnect loop handles recovery.
            }
        }
    }

    private fun appLabel(pkg: String): String {
        return try {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(pkg, 0)).toString()
        } catch (_: Exception) {
            pkg
        }
    }

    override fun onDestroy() {
        controlJob?.cancel()
        scope.cancel()
        super.onDestroy()
    }

    companion object {
        private const val DEDUP_MS = 30_000L
        private const val MIN_GAP_MS = 2_000L
        private val lock = Any()
        private var lastKey = ""
        private var lastKeyTime = 0L
        private var lastForward = 0L

        private val DENYLIST = setOf(
            "android",
            "com.android.systemui",
            "com.android.phone",
            "com.android.server.telecom",
            "com.hikaboshi.companion",
        )
    }
}
