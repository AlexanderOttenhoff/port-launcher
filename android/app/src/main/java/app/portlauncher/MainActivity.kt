package app.portlauncher

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue
import app.portlauncher.library.LibraryController
import app.portlauncher.ui.LauncherTheme
import app.portlauncher.ui.LibraryScreen

class MainActivity : ComponentActivity() {
    private val library by lazy { LibraryController(applicationContext) }

    /**
     * "Add a mod from a file" on a game page's Mods tab (owner 2026-11-16): the system file picker. Registered here
     * (activity results must be registered before the activity starts); [pickFile] hands the result to its caller.
     */
    private var pickCallback: ((android.net.Uri?) -> Unit)? = null
    private val pickModFile = registerForActivityResult(androidx.activity.result.contract.ActivityResultContracts.OpenDocument()) { uri ->
        val callback = pickCallback
        pickCallback = null
        callback?.invoke(uri)
    }

    fun pickFile(callback: (android.net.Uri?) -> Unit) {
        pickCallback = callback
        runCatching { pickModFile.launch(arrayOf("*/*")) }.onFailure { pickCallback = null; callback(null) }
    }

    // Counts resumes, so the library can put controller focus back after a game or the folder picker.
    private var resumes by mutableIntStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // A fresh start from the home-screen icon opens on the bare library (owner 2026-11-16), not on the game page
        // remembered from last time. Recreated after the system reclaimed it (savedInstanceState set, e.g. coming
        // back from a long game) it keeps the page, like any other window switch.
        if (savedInstanceState == null && isLauncherOpen(intent)) library.closeMenus()
        enableEdgeToEdge()
        setContent {
            LauncherTheme {
                // The first-launch build of a split recomp module (ROADMAP item 13) takes over the whole screen.
                library.prepare?.let { state ->
                    app.portlauncher.ui.PreparingScreen(state, onCancel = library::cancelPrepare,
                        onClose = { library.dismissPrepare(); resumes++ })
                    return@LauncherTheme
                }
                // Mods live on each game page's Mods tab (owner 2026-11-15); the separate mods screen is gone.
                LibraryScreen(library, resumes)
            }
        }
    }

    /**
     * The home-screen icon while the library is already running (owner 2026-11-16): close every menu and land on the
     * library. Only this path gets here: launchMode singleTop makes Android deliver the icon's MAIN/LAUNCHER intent
     * when this activity is the task's top, whereas switching back through Recents brings the task forward without
     * an intent (menus stay, as for any window switch). With a game on top the icon just brings the game back,
     * untouched, so in-game nothing changes.
     */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (isLauncherOpen(intent)) library.closeMenus()
    }

    private fun isLauncherOpen(intent: Intent?): Boolean =
        intent?.action == Intent.ACTION_MAIN && intent.hasCategory(Intent.CATEGORY_LAUNCHER)

    override fun onResume() {
        super.onResume()
        resumes++
    }
}
