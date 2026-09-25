package com.hikaboshi.companion.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.BrightnessHigh
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.NotificationsActive
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Send
import androidx.compose.material.icons.filled.Smartphone
import androidx.compose.material.icons.filled.SystemUpdate
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WatchApp(
    vm: WatchViewModel,
    state: UiState,
    onRequestLocationPermission: ((Boolean) -> Unit) -> Unit,
) {
    val snackbar = remember { SnackbarHostState() }

    if (state.showAppPicker) {
        AppPickerDialog(state, vm)
    }

    LaunchedEffect(state.toast) {
        state.toast?.let {
            snackbar.showSnackbar(it)
            vm.clearToast()
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("Hikaboshi")
                        Text(
                            when {
                                state.ble.connected -> "Connected ${state.ble.address ?: ""}"
                                state.ble.connecting -> "Connecting…"
                                state.ble.reconnecting -> "Reconnecting…"
                                else -> "Disconnected"
                            },
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                },
                actions = {
                    if (state.ble.connected) {
                        IconButton(onClick = { vm.refresh() }) {
                            Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                        }
                        IconButton(onClick = { vm.disconnect() }) {
                            Icon(Icons.Default.Close, contentDescription = "Disconnect")
                        }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 12.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            LinkSettingsCard(state, vm)
            NotificationSettingsCard(state, vm)
            if (!state.ble.connected) {
                ScanSection(state, vm)
            } else {
                DashboardSection(state, vm, onRequestLocationPermission)
            }
        }
    }
}

@Composable
private fun LinkSettingsCard(state: UiState, vm: WatchViewModel) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Link", style = MaterialTheme.typography.titleMedium)
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text("Auto-connect")
                    Text(
                        "Reconnect in background and after reboot",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                Switch(
                    checked = state.ble.autoConnect,
                    onCheckedChange = { vm.setAutoConnect(it) },
                )
            }
            if (state.ble.lastAddress != null) {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "Remembered: ${state.ble.lastAddress}",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                    OutlinedButton(onClick = { vm.forgetDevice() }) { Text("Forget") }
                }
            }
        }
    }
}

@Composable
private fun NotificationSettingsCard(state: UiState, vm: WatchViewModel) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Phone notifications", style = MaterialTheme.typography.titleMedium)
            if (!state.listenerGranted) {
                Text(
                    "Grant notification access to forward phone notifications to the watch.",
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedButton(onClick = { vm.openListenerSettings() }) {
                    Icon(Icons.Default.NotificationsActive, null)
                    Text("  Grant access")
                }
            } else {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("Forward to watch")
                        Text(
                            "Sends title + text of new phone notifications",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    Switch(
                        checked = state.forwardEnabled,
                        onCheckedChange = { vm.setForwarding(it) },
                    )
                }
                OutlinedButton(onClick = { vm.openAppPicker() }) {
                    Icon(Icons.Default.Smartphone, null)
                    Text("  Choose apps…")
                }
            }
        }
    }
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Phone Silent (from watch)", style = MaterialTheme.typography.titleMedium)
            if (!state.dndAccessGranted) {
                Text(
                    "Grant Do Not Disturb access so the watch's Phone Silent button can mute this phone.",
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedButton(onClick = { vm.openDndSettings() }) {
                    Icon(Icons.Default.NotificationsActive, null)
                    Text("  Grant access")
                }
            } else {
                Text(
                    "Enabled — the watch's Phone Silent button toggles this phone's Do Not Disturb.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
private fun AppPickerDialog(state: UiState, vm: WatchViewModel) {
    AlertDialog(
        onDismissRequest = { vm.closeAppPicker() },
        title = { Text("Forward notifications from") },
        text = {
            if (state.notifApps.isEmpty()) {
                Text("Loading apps…", style = MaterialTheme.typography.bodySmall)
            } else {
                LazyColumn(Modifier.heightIn(max = 420.dp)) {
                    items(state.notifApps, key = { it.packageName }) { app ->
                        Row(
                            Modifier.fillMaxWidth().padding(vertical = 6.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(app.label, Modifier.weight(1f))
                            Switch(
                                checked = app.enabled,
                                onCheckedChange = { vm.setNotifAppEnabled(app.packageName, it) },
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { vm.closeAppPicker() }) { Text("Done") }
        },
    )
}

@Composable
private fun ScanSection(state: UiState, vm: WatchViewModel) {
    Column(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().padding(top = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Devices", style = MaterialTheme.typography.titleMedium)
            Row {
                OutlinedButton(onClick = { vm.scan() }, enabled = !state.ble.scanning) {
                    Icon(Icons.Default.Bluetooth, null)
                    Spacer(Modifier.height(4.dp))
                    Text(if (state.ble.scanning) "Scanning…" else "Scan")
                }
                if (state.ble.scanning) {
                    Spacer(Modifier.padding(4.dp))
                    OutlinedButton(onClick = { vm.stopScan() }) { Text("Stop") }
                }
            }
        }

        state.ble.error?.let { err ->
            Text(
                err,
                modifier = Modifier.padding(top = 8.dp),
                color = MaterialTheme.colorScheme.error,
            )
        }

        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            state.ble.scanResults.forEach { dev ->
                Card(Modifier.fillMaxWidth()) {
                    Row(
                        Modifier.fillMaxWidth().padding(12.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column {
                            Text(dev.name ?: "(no name)", style = MaterialTheme.typography.titleSmall)
                            Text(dev.address, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                            Text("RSSI ${dev.rssi}", style = MaterialTheme.typography.bodySmall)
                        }
                        Button(
                            onClick = { vm.connect(dev.address) },
                            enabled = !state.busy,
                        ) { Text("Connect") }
                    }
                }
            }
        }

        if (state.ble.scanResults.isEmpty() && !state.ble.scanning) {
            Text(
                "Wake the watch (raise wrist) then tap Scan.\nAdvertising stops after ~5 min idle.",
                Modifier.padding(top = 24.dp),
                style = MaterialTheme.typography.bodyMedium,
            )
        }

        EventLogCard(state, Modifier.padding(top = 12.dp))
    }
}

@Composable
private fun EventLogCard(state: UiState, modifier: Modifier = Modifier) {
    Card(modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Text("Event log", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(4.dp))
            if (state.history.isEmpty() && state.ble.log.isEmpty()) {
                Text("—", style = MaterialTheme.typography.bodySmall)
            } else {
                val lines = (state.ble.log + state.history).takeLast(40).reversed()
                lines.forEach { line ->
                    Text(
                        line,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
        }
    }
}

@Composable
private fun DashboardSection(
    state: UiState,
    vm: WatchViewModel,
    onRequestLocationPermission: ((Boolean) -> Unit) -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // Status
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Status", style = MaterialTheme.typography.titleMedium)
                Text("Battery: ${state.ble.battery?.let { "$it%" } ?: "—"}")
                Text("Steps: ${state.ble.steps?.toString() ?: "—"}")
                Text(
                    "Distance: ${
                        state.ble.distanceM?.let { m ->
                            if (m >= 1000) "%.2f km".format(m / 1000.0) else "$m m"
                        } ?: "—"
                    }",
                )
                Text(
                    "Calories: ${
                        state.ble.caloriesKcal?.let { "%.1f kcal".format(it) } ?: "—"
                    }",
                )
                state.ble.mtu?.let { Text("MTU: $it") }
                state.ble.lastNotification?.let { n ->
                    Text("Last notif: ${n.title} — ${n.body}")
                }
                state.ble.lastOtaStatus?.let {
                    Text("OTA: $it ${state.ble.lastOtaProgress?.let { p -> "($p%)" } ?: ""}")
                }
            }
        }

        // Send notification
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Send notification", style = MaterialTheme.typography.titleMedium)
                OutlinedTextField(
                    value = state.notifTitle,
                    onValueChange = vm::setNotifTitle,
                    label = { Text("Title") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = state.notifBody,
                    onValueChange = vm::setNotifBody,
                    label = { Text("Body") },
                    modifier = Modifier.fillMaxWidth(),
                )
                Button(onClick = vm::sendNotification, enabled = !state.busy) {
                    Icon(Icons.Default.Send, null)
                    Text("  Send")
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(
                        onClick = { vm.sendPreset("👋 Test notification") },
                        enabled = !state.busy,
                    ) { Text("Test") }
                    OutlinedButton(
                        onClick = { vm.sendPreset("🔔 Ping") },
                        enabled = !state.busy,
                    ) { Text("Ping") }
                }
            }
        }

        // Controls
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Controls", style = MaterialTheme.typography.titleMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { vm.screen(true) }, enabled = !state.busy) {
                        Icon(Icons.Default.BrightnessHigh, null); Text(" Screen on")
                    }
                    OutlinedButton(onClick = { vm.screen(false) }, enabled = !state.busy) {
                        Icon(Icons.Default.Close, null); Text(" Screen off")
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { vm.wifi(true) }, enabled = !state.busy) {
                        Icon(Icons.Default.Wifi, null); Text(" WiFi on")
                    }
                    OutlinedButton(onClick = { vm.wifi(false) }, enabled = !state.busy) {
                        Icon(Icons.Default.Wifi, null); Text(" WiFi off")
                    }
                }
                OutlinedButton(onClick = vm::setTimeSync, enabled = !state.busy) {
                    Icon(Icons.Default.Check, null); Text(" Sync time")
                }
            }
        }

        // Weather
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Weather", style = MaterialTheme.typography.titleMedium)
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("Auto (device location)")
                        Text(
                            "Fetches and pushes on connect + hourly",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    Switch(
                        checked = state.ble.autoWeather,
                        onCheckedChange = { checked ->
                            if (checked && !vm.hasLocationPermission()) {
                                onRequestLocationPermission { granted ->
                                    if (granted) vm.setAutoWeather(true)
                                }
                            } else {
                                vm.setAutoWeather(checked)
                            }
                        },
                    )
                }
                if (state.ble.autoWeather) {
                    OutlinedButton(onClick = { vm.refreshWeatherNow() }, enabled = !state.busy) {
                        Icon(Icons.Default.Refresh, null); Text(" Refresh now")
                    }
                }
                Text("Manual override", style = MaterialTheme.typography.labelMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = state.weatherTemp,
                        onValueChange = vm::setWeatherTemp,
                        label = { Text("°C") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    OutlinedTextField(
                        value = state.weatherCode,
                        onValueChange = vm::setWeatherCode,
                        label = { Text("Code") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                }
                Button(onClick = vm::sendWeather, enabled = !state.busy) {
                    Icon(Icons.Default.Cloud, null); Text("  Push weather")
                }
            }
        }

        // OTA
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("OTA firmware", style = MaterialTheme.typography.titleMedium)
                OutlinedTextField(
                    value = state.otaUrl,
                    onValueChange = vm::setOtaUrl,
                    label = { Text("https://…/smart_watch.bin") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                Button(onClick = vm::sendOta, enabled = !state.busy) {
                    Icon(Icons.Default.SystemUpdate, null); Text("  Start OTA")
                }
                state.ble.lastOtaProgress?.let { p ->
                    Text("Progress: $p%")
                }
            }
        }

        EventLogCard(state)

        // Find phone / music hint
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp)) {
                Text("From the watch", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Find Phone → rings this phone.\n" +
                        "Music Control → play/next/prev media keys.\n" +
                        "Alarm → shows a toast + event log entry.\n" +
                        "Notification screen, OK → dismisses it on the phone too.\n" +
                        "Phone Silent (root menu) → toggles this phone's DND.\n" +
                        "Keep this app open (or the link notification) for events.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}
