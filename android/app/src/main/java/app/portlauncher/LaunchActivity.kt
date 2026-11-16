package app.portlauncher

import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.unit.dp
import androidx.lifecycle.lifecycleScope
import app.portlauncher.library.LibraryController
import app.portlauncher.ui.FocusButton
import app.portlauncher.ui.LauncherTheme
import kotlinx.coroutines.launch

/**
 * Launch target for frontends such as ES-DE, Argosy and Daijisho (ROADMAP "Launch target for frontends"): they send
 * ACTION_VIEW with the ROM as a content:// URI, the same convention standalone emulators use. A supported ROM starts
 * its game straight away (this activity then finishes, so Back from the game returns to the frontend); anything
 * else, the usual case, gets a short explanation and the list of games the launcher can run.
 */
class LaunchActivity : ComponentActivity() {
    private val library by lazy { LibraryController(applicationContext) }
    private var problem by mutableStateOf<LibraryController.NotPlayable?>(null)
    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val uri = intent?.data
        setContent {
            LauncherTheme {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    val p = problem
                    // A frontend launch of a split recomp module builds its game code first, same screen as the
                    // library's; Cancel or closing the error ends this activity (back to the frontend; otherwise a
                    // cancelled build would leave the "Starting" spinner up with nothing behind it).
                    val prep = library.prepare
                    if (prep != null) app.portlauncher.ui.PreparingScreen(prep, onCancel = { library.cancelPrepare(); finish() },
                        onClose = { library.dismissPrepare(); finish() })
                    else if (p == null) Starting() else NotPlayableScreen(p, library, onClose = { finish() }, onLibrary = {
                        startActivity(android.content.Intent(this, MainActivity::class.java))
                        finish()
                    })
                }
            }
        }
        if (uri == null) {
            problem = LibraryController.NotPlayable("", "No ROM was passed to the launcher.")
            return
        }
        lifecycleScope.launch {
            problem = library.playUri(this@LaunchActivity, uri, displayName(uri))
            started = problem == null
        }
    }

    override fun onStop() {
        super.onStop()
        // The game is running in its own activity now; nothing to come back to here.
        if (started) finish()
    }

    private fun displayName(uri: Uri): String = runCatching {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    }.getOrNull() ?: uri.lastPathSegment?.substringAfterLast('/') ?: "ROM"
}

@androidx.compose.runtime.Composable
private fun Starting() {
    Column(
        modifier = Modifier.fillMaxSize(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) { CircularProgressIndicator() }
}

@androidx.compose.runtime.Composable
private fun NotPlayableScreen(
    problem: LibraryController.NotPlayable,
    library: LibraryController,
    onClose: () -> Unit,
    onLibrary: () -> Unit,
) {
    val focus = remember { FocusRequester() }
    Column(
        modifier = Modifier.fillMaxSize().safeDrawingPadding().padding(horizontal = 24.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(if (problem.fileName.isEmpty()) "Can't start a game" else "Can't play ${problem.fileName}",
            style = MaterialTheme.typography.headlineSmall)
        Text(problem.reason, style = MaterialTheme.typography.bodyLarge)
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            FocusButton(text = "Close", modifier = Modifier.focusRequester(focus), onClick = onClose)
            FocusButton(text = "Open the library", outlined = true, onClick = onLibrary)
        }
        Text("Games the launcher can run", style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = 8.dp))
        LazyColumn(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            items(library.catalog.games()) { game ->
                Text("${game.title} · ${game.region} ${game.revision} · ${game.modules.joinToString { library.catalog.moduleName(it) }}",
                    style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
}
