package com.hikaboshi.companion.ble

import android.app.Notification
import android.content.ComponentName
import android.app.NotificationManager
import android.media.MediaMetadata
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
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
/** Now-playing state read from the active media session. */
data class NowPlaying(
    val title: String?,
    val artist: String?,
    val playing: Boolean,
    val hasSession: Boolean,
) {
    companion object {
        val NONE = NowPlaying(null, null, false, false)
    }
}

class WatchNotificationListener : NotificationListenerService() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var controlJob: Job? = null


    /** System key of the last notification actually forwarded; target of a watch dismiss. */
    private var lastForwardedSbnKey: String? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        isConnected = true
        Log.d(TAG, "notification listener connected; media sessions readable")
        hooked.clear()
        ensureCallbacks()
        controlJob?.cancel()
        controlJob = scope.launch {
            BleHolder.get(applicationContext).controlEvents.collect { cmd -> handleControl(cmd) }
        }
    }

    override fun onListenerDisconnected() {
        isConnected = false
        controlJob?.cancel()
        controlJob = null
        super.onListenerDisconnected()
    }


    /** Read the active media session. Only valid on a bound listener service. */
    private var hooked = mutableSetOf<String>()
    private var lastLoggedOnce: String? = null

    private fun logOnce(msg: String) {
        if (msg != lastLoggedOnce) {
            lastLoggedOnce = msg
            Log.w(TAG, msg)
        }
    }

    private fun ensureCallbacks() {
        val mgr = getSystemService(MediaSessionManager::class.java) ?: return
        val self = ComponentName(this, WatchNotificationListener::class.java)
        val sessions = try {
            mgr.getActiveSessions(self)
        } catch (e: Exception) {
            emptyList()
        }
        for (c in sessions) {
            val key = c.sessionToken.toString()
            if (!hooked.add(key)) continue
            try {
                c.registerCallback(callback, Handler(Looper.getMainLooper()))
                Log.d(TAG, "registered media callback for $key")
            } catch (e: Exception) {
                logOnce("registerCallback failed: ${e.message}")
                hooked.remove(key)
            }
        }
    }

    private val callback = object : android.media.session.MediaController.Callback() {
        override fun onPlaybackStateChanged(state: PlaybackState?) = emitMediaChanged()
        override fun onMetadataChanged(meta: MediaMetadata?) = emitMediaChanged()
        override fun onSessionDestroyed() {
            hooked.clear()
            emitMediaChanged()
        }
    }

    private fun emitMediaChanged() {
        val np = queryNowPlaying() ?: return
        onMediaChanged?.invoke(np)
    }

    private fun queryNowPlaying(): NowPlaying? {
        val mgr = getSystemService(MediaSessionManager::class.java) ?: return null
        // Explicit method call: the property accessor resolves ambiguously
        // against the NotificationListenerService overloads.
        // Passing our own listener component is what identifies the caller as an
        // authorised media observer; a null ComponentName is rejected with
        // "Missing permission to control media" even from a bound listener.
        val self = ComponentName(this, WatchNotificationListener::class.java)
        val sessions: List<android.media.session.MediaController> = try {
            mgr.getActiveSessions(self)
        } catch (e: Exception) {
            Log.w(TAG, "getActiveSessions(self) failed: ${e.message}")
            try {
                mgr.getActiveSessions(null)
            } catch (e2: Exception) {
                Log.w(TAG, "getActiveSessions(null) failed: ${e2.message}")
                return null
            }
        }
        if (sessions.isEmpty()) {
            Log.w(TAG, "getActiveSessions returned 0 sessions")
            return NowPlaying.NONE
        }

        // Several sessions can be live at once (e.g. a stale notification
        // session). Prefer one that is playing, and among those one that has
        // metadata, so we never latch onto an empty placeholder.
        val playing = sessions.filter { it.playbackState?.state == PlaybackState.STATE_PLAYING }
        val pool = playing.ifEmpty { sessions }
        val withMeta = pool.filter { it.metadata != null }
        val chosen = (withMeta.ifEmpty { pool }).first()

        val meta = chosen.metadata
        val title = meta?.getString(MediaMetadata.METADATA_KEY_TITLE)
            ?: meta?.getString(MediaMetadata.METADATA_KEY_DISPLAY_TITLE)
        val artist = meta?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: meta?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST)
        return NowPlaying(
            title = title,
            artist = artist,
            playing = chosen.playbackState?.state == PlaybackState.STATE_PLAYING,
            hasSession = true,
        )
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
        isConnected = false
        instance = null
        super.onDestroy()
    }

    companion object {
        private const val TAG = "HikaboshiNLS"

        /**
         * This service is bound only while the user has granted notification
         * listener access, and being a NotificationListenerService is what allows
         * MediaSessionManager.getActiveSessions() to return anything. The same call
         * from the link service returns an empty list, which is why the media read
         * has to happen here.
         */
        @Volatile
        private var instance: WatchNotificationListener? = null

        @Volatile
        var isConnected: Boolean = false
            private set

        /**
         * Set by the link service to receive pushed media updates. Invoked on
         * the main thread whenever a session's metadata or playback state
         * changes, so the watch does not have to wait for a poll.
         */
        @Volatile
        var onMediaChanged: ((NowPlaying) -> Unit)? = null

        /** Read the active media session. Safe to call from any thread. */
        fun readNowPlaying(): NowPlaying? {
            val svc = instance
            if (svc == null || !isConnected) return null
            return svc.queryNowPlaying()
        }

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
