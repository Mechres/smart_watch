package com.hikaboshi.companion.ble

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.RingtoneManager
import android.os.Build
import android.os.IBinder
import android.view.KeyEvent
import androidx.core.app.NotificationCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.hikaboshi.companion.MainActivity
import com.hikaboshi.companion.R
import kotlinx.coroutines.launch

/**
 * Foreground service keeping the BLE link alive while the app is backgrounded.
 * Hosts find-phone ringing and media key dispatch for watch-initiated events.
 */
class WatchLinkService : LifecycleService() {

    companion object {
        const val EXTRA_ADDRESS = "address"
        private const val CHANNEL_ID = "hikaboshi_link"
        private const val NOTIF_ID = 42

        @Volatile
        var instance: WatchLinkService? = null
            private set

        /** Shared manager owned by the UI — service must never open its own GATT. */
        @Volatile
        var sharedManager: WatchBleManager? = null

        fun start(context: Context, address: String, manager: WatchBleManager? = null) {
            if (manager != null) sharedManager = manager
            val intent = Intent(context, WatchLinkService::class.java)
                .putExtra(EXTRA_ADDRESS, address)
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, WatchLinkService::class.java))
        }
    }

    private var ble: WatchBleManager? = null
    private var ringing = false

    override fun onCreate() {
        super.onCreate()
        instance = this
        createChannel()
        sharedManager?.let { attachManager(it) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        startForeground(NOTIF_ID, buildNotification("Connecting…"))

        sharedManager?.let { attachManager(it) }
        val manager = ble
        val address = intent?.getStringExtra(EXTRA_ADDRESS)
            ?: manager?.state?.value?.address

        if (manager != null && manager.state.value.connected) {
            updateNotification("Connected to Hikaboshi")
            return START_NOT_STICKY
        }
        if (manager != null && manager.state.value.connecting) {
            return START_NOT_STICKY
        }
        // Never open a second GATT client (watch allows only 1 connection).
        if (manager == null) {
            updateNotification("Waiting for app connection…")
            return START_NOT_STICKY
        }
        if (address != null) {
            ensureBle(address)
        }
        return START_NOT_STICKY
    }

    private fun ensureBle(address: String) {
        val manager = ble ?: return
        manager.onControlEvent = { cmd -> handleControl(cmd) }
        if (manager.state.value.connected) {
            updateNotification("Connected to Hikaboshi")
            return
        }
        if (manager.state.value.connecting) return
        lifecycleScope.launch {
            try {
                manager.connect(address)
                updateNotification("Connected to Hikaboshi")
            } catch (e: Exception) {
                updateNotification("Connection failed: ${e.message}")
            }
        }
    }

    fun attachManager(manager: WatchBleManager) {
        ble = manager
        sharedManager = manager
        manager.onControlEvent = { cmd -> handleControl(cmd) }
    }

    private fun handleControl(cmd: String) {
        when (cmd) {
            Protocol.EVT_FIND_PHONE -> startRinging()
            Protocol.EVT_FIND_PHONE_STOP -> stopRinging()
            Protocol.EVT_MUSIC_TOGGLE -> sendMedia(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)
            Protocol.EVT_MUSIC_NEXT -> sendMedia(KeyEvent.KEYCODE_MEDIA_NEXT)
            Protocol.EVT_MUSIC_PREV -> sendMedia(KeyEvent.KEYCODE_MEDIA_PREVIOUS)
        }
    }

    private fun startRinging() {
        if (ringing) return
        ringing = true
        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_NOTIFICATION)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build()
        // Loop via MediaPlayer would be ideal; a loud ringtone blast is enough for v1.
        val player = android.media.MediaPlayer.create(this, uri)
        player?.setAudioAttributes(attrs)
        player?.setOnCompletionListener { if (ringing) startRinging() }
        player?.start()
        updateNotification("Ringing… (from watch)")
    }

    private fun stopRinging() {
        ringing = false
        updateNotification("Connected to Hikaboshi")
    }

    private fun sendMedia(keycode: Int) {
        val ev = KeyEvent(KeyEvent.ACTION_DOWN, keycode)
        dispatchMediaKeyEvent(ev)
        dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, keycode))
    }

    private fun dispatchMediaKeyEvent(ev: KeyEvent) {
        val audio = getSystemService(AUDIO_SERVICE) as android.media.AudioManager
        audio.dispatchMediaKeyEvent(ev)
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Watch link",
                NotificationManager.IMPORTANCE_LOW,
            )
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIF_ID, buildNotification(text))
    }

    override fun onBind(intent: Intent): IBinder? {
        super.onBind(intent)
        return null
    }

    override fun onDestroy() {
        instance = null
        if (ble === sharedManager) {
            // UI owns the shared manager — don't tear it down here.
            ble = null
        } else {
            ble?.disconnect()
            ble = null
        }
        super.onDestroy()
    }
}
