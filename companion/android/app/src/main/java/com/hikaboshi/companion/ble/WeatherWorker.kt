package com.hikaboshi.companion.ble

import android.content.Context
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

/**
 * Periodic background refresh so weather stays current even if the watch
 * stays connected for hours without a reconnect (which is when the BLE
 * manager's own connect-time fetch would otherwise fire).
 */
class WeatherWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val ble = BleHolder.get(applicationContext)
        val state = ble.state.value
        if (!state.autoWeather || !state.connected) return Result.success()
        return if (ble.refreshWeatherNow()) Result.success() else Result.retry()
    }

    companion object {
        private const val UNIQUE_NAME = "weather_refresh"

        /** Cheap to call repeatedly; the worker itself no-ops when auto-weather is off. */
        fun ensureScheduled(context: Context) {
            val request = PeriodicWorkRequestBuilder<WeatherWorker>(60, TimeUnit.MINUTES)
                .setConstraints(
                    Constraints.Builder()
                        .setRequiredNetworkType(NetworkType.CONNECTED)
                        .build(),
                )
                .build()
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                UNIQUE_NAME,
                ExistingPeriodicWorkPolicy.KEEP,
                request,
            )
        }
    }
}
