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
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

/**
 * Foreground service keeping the BLE link alive while the app is backgrounded.
 * Hosts find-phone ringing and media key dispatch for watch-initiated events.
 * START_STICKY + remembered device = link survives reboots and process kills.
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
    private var stateJob: Job? = null
    private var controlJob: Job? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        createChannel()
        val mgr = sharedManager ?: BleHolder.get(this).also { sharedManager = it }
        attachManager(mgr)
        // Keep the foreground notification truthful without manual updates.
        stateJob?.cancel()
        stateJob = lifecycleScope.launch {
            mgr.state.collect { s ->
                updateNotification(
                    when {
                        s.connected -> "Connected to Hikaboshi"
                        s.connecting || s.reconnecting -> "Connecting to watch…"
                        else -> "Disconnected — tap to open app"
                    },
                )
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        startForeground(NOTIF_ID, buildNotification("Starting…"))

        val mgr = ble ?: BleHolder.get(this).also { attachManager(it) }
        val address = intent?.getStringExtra(EXTRA_ADDRESS)
        lifecycleScope.launch {
            if (address != null && !mgr.state.value.connected && !mgr.state.value.connecting) {
                try {
                    mgr.connect(address)
                } catch (e: Exception) {
                    // connect() failure still leaves the reconnect loop running
                    // when auto-connect is on.
                }
            } else {
                mgr.autoConnectIfRemembered()
            }
        }
        return START_STICKY
    }

    fun attachManager(manager: WatchBleManager) {
        ble = manager
        sharedManager = manager
        controlJob?.cancel()
        controlJob = lifecycleScope.launch {
            manager.controlEvents.collect { cmd -> handleControl(cmd) }
        }
    }

    private fun handleControl(cmd: String) {
        when (cmd) {
            Protocol.EVT_FIND_PHONE -> startRinging()
            Protocol.EVT_FIND_PHONE_STOP -> stopRinging()
            Protocol.EVT_MUSIC_TOGGLE -> sendMedia(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)
            Protocol.EVT_MUSIC_NEXT -> sendMedia(KeyEvent.KEYCODE_MEDIA_NEXT)
            Protocol.EVT_MUSIC_PREV -> sendMedia(KeyEvent.KEYCODE_MEDIA_PREVIOUS)
            Protocol.EVT_ALARM -> updateNotification("Alarm! (from watch)")
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
        stateJob?.cancel()
        stateJob = null
        controlJob?.cancel()
        controlJob = null
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
