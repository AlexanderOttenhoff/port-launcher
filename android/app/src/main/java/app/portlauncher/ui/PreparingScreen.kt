package app.portlauncher.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.portlauncher.library.PrepareState

/**
 * The first-launch "Preparing <game> — one time only" screen (ROADMAP item 13, docs/planning/ON_DEVICE_RECOMP.md):
 * full screen, because for a minute or more there's nothing else the player should do, and the library's one-line
 * status would read like a hang. Three states (PrepareState): installing a side-loaded toolkit (debug builds),
 * building the game code, and the error screen with the build log's tail.
 *
 * Controller first, like the rest of the launcher: the one button always has focus. Back does nothing while
 * building (an accidental Back would throw away minutes of work; Cancel is an explicit choice) and closes the error.
 */
@Composable
fun PreparingScreen(state: PrepareState, onCancel: () -> Unit, onClose: () -> Unit) {
    val focus = remember { FocusRequester() }
    BackHandler { if (state is PrepareState.Failed) onClose() }
    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        when (state) {
            is PrepareState.Failed -> Column(
                modifier = Modifier.fillMaxSize().safeDrawingPadding().padding(horizontal = 24.dp, vertical = 16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text("Couldn't prepare ${state.title}", style = MaterialTheme.typography.headlineSmall)
                Text(state.message, style = MaterialTheme.typography.bodyLarge)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    FocusButton(text = "Back", modifier = Modifier.focusRequester(focus), onClick = onClose)
                }
                if (state.logTail.isNotBlank()) {
                    // The tail is for whoever debugs it (us, for now): monospace, small, scrollable, newest at the
                    // bottom. The full log is at state.logPath, said in the last line.
                    Column(modifier = Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState())) {
                        Text(state.logTail, fontFamily = FontFamily.Monospace, fontSize = 11.sp, lineHeight = 13.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                if (state.logPath.isNotEmpty()) {
                    Text("Full log: ${state.logPath}", style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            else -> Column(
                modifier = Modifier.fillMaxSize().safeDrawingPadding().padding(horizontal = 48.dp, vertical = 24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
            ) {
                val (headline, phase, fraction) = when (state) {
                    is PrepareState.Installing -> Triple("Installing the build tools", "Unpacking the toolkit", state.fraction)
                    is PrepareState.Building -> Triple("Preparing ${state.title}", state.phase, state.fraction)
                    else -> Triple("", "", 0f)
                }
                Text(headline, style = MaterialTheme.typography.headlineMedium, textAlign = TextAlign.Center)
                Text("One time only. Your device is building the game from your own copy; next time it starts right away.",
                    style = MaterialTheme.typography.bodyLarge, textAlign = TextAlign.Center,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                LinearProgressIndicator(progress = { fraction }, modifier = Modifier.widthIn(max = 560.dp).fillMaxWidth())
                Text(phase, style = MaterialTheme.typography.bodyMedium)
                // No Cancel while unpacking the toolkit: it takes seconds, and stopping halfway would only leave a
                // temp dir to clean up.
                if (state is PrepareState.Building) {
                    FocusButton(text = "Cancel", outlined = true, modifier = Modifier.focusRequester(focus), onClick = onCancel)
                }
            }
        }
    }
    // Re-request when the state's kind changes: the error screen's Back button replaces the Cancel button.
    LaunchedEffect(state::class) { runCatching { focus.requestFocus() } }
}
