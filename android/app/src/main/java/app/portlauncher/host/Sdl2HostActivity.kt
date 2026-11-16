package app.portlauncher.host

import app.portlauncher.util.writeTextAtomic
import android.content.Intent
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import app.portlauncher.core.CoreBridge
import java.lang.ref.WeakReference
import app.portlauncher.sdl2.SDLActivity

/**
 * Runs one game module in the ":game" process (docs/ARCHITECTURE.md, "Game process"). SDL's Java glue loads
 * lib<module>.so, whose statically linked SDL registers its JNI methods for app.portlauncher.sdl2, then calls the
 * module's plm_main with the launch.json path.
 */
class Sdl2HostActivity : SDLActivity() {
    companion object {
        const val EXTRA_MODULE_ID = "app.portlauncher.extra.MODULE_ID"
        const val EXTRA_LAUNCH_JSON = "app.portlauncher.extra.LAUNCH_JSON"
        const val EXTRA_TITLE = "app.portlauncher.extra.TITLE"
        const val EXTRA_DIRECT = "app.portlauncher.extra.DIRECT"
        /** The module's mods (ModsSpec JSON: curated list, sources, built-in mods), for the in-game Mods tab. */
        const val EXTRA_MODS_SPEC = "app.portlauncher.extra.MODS_SPEC"
        private const val REQUEST_PICK_MOD = 4101
        /** Full path of a downloaded module's library (module server); absent for modules bundled in the APK. */
        const val EXTRA_MODULE_LIB = "app.portlauncher.extra.MODULE_LIB"

        // How long a module gets to save and return after plm_request_quit before the host closes it.
        private const val QUIT_TIMEOUT_MS = 8000L
        // When to explain a long first start (shader preparation), and when to give up waiting for a first frame.
        private const val SLOW_START_HINT_MS = 6000L
        private const val LOADING_TIMEOUT_MS = 180_000L

        private var current = WeakReference<Sdl2HostActivity>(null)

        /** Host API callback (CoreBridge.attachHost): the module drew its first frame. Any thread. */
        @JvmStatic
        fun onModuleFirstFrame() {
            val activity = current.get() ?: return
            Log.i(TAG, "first frame ${SystemClock.elapsedRealtime() - activity.startedAt} ms after start")
            activity.runOnUiThread { activity.hideLoading() }
        }

        private const val TAG = "plm-host"
    }

    private var menu: GameMenu? = null
    private var loading: View? = null
    private var hostAttached = false
    private val startedAt = SystemClock.elapsedRealtime()

    private fun moduleId(): String =
        requireNotNull(intent.getStringExtra(EXTRA_MODULE_ID)) { "no module id in the launch intent" }

    // SDL loads these in order and treats the last one as the main library (libplm_<id>.so).
    override fun getLibraries(): Array<String> = arrayOf("plm_${moduleId()}")

    override fun getMainFunction(): String = "plm_main"

    /**
     * The module's library as the dynamic linker knows it: the full path for a downloaded module, the plain name
     * for one bundled in the APK. Every CoreBridge call (dlopen with RTLD_NOLOAD) uses this, so both kinds work.
     */
    private val moduleLib: String by lazy { intent.getStringExtra(EXTRA_MODULE_LIB) ?: "libplm_${moduleId()}.so" }

    // SDL looks up plm_main in this library; for a downloaded module that's its path in app storage.
    override fun getMainSharedObject(): String =
        intent.getStringExtra(EXTRA_MODULE_LIB) ?: super.getMainSharedObject()

    override fun loadLibraries() {
        // Environment from launch.json before the module loads: its global constructors may already read it.
        // Also written to files/plm-env for code running under ARM translation (the emulator), whose C library
        // doesn't see variables set from Java; the module shim reads it before the port's constructors.
        launchJson?.optJSONObject("env")?.let { env ->
            val lines = StringBuilder()
            for (key in env.keys()) {
                runCatching { android.system.Os.setenv(key, env.getString(key), true) }
                    .onFailure { Log.w(TAG, "setenv $key: ${it.message}") }
                lines.append(key).append('=').append(env.getString(key)).append('\n')
            }
            // Atomic (util/AtomicFiles.kt): the shim reads this file during library load; never let it see half of it.
            runCatching { java.io.File(filesDir, "plm-env").writeTextAtomic(lines.toString()) }
        }
        val downloaded = intent.getStringExtra(EXTRA_MODULE_LIB)
        if (downloaded != null) System.load(downloaded) else super.loadLibraries()
        current = WeakReference(this)
        // Modules with the host API report their first frame; only then can a loading screen be removed reliably.
        hostAttached = CoreBridge.attachHost(moduleLib, Sdl2HostActivity::class.java)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        refreshLaunchSettings()
        super.onCreate(savedInstanceState)
        hideSystemBars()
        if (hostAttached && intent.getBooleanExtra(EXTRA_DIRECT, true)) {
            showLoading()
        }
    }

    // Landscape only (owner 2026-11-15), whatever the port's window asks for: SDL would otherwise pick an orientation
    // from the window size and its hint (seen as portrait on a phone-shaped screen), overriding the manifest's lock.
    override fun setOrientationBis(w: Int, h: Int, resizable: Boolean, hint: String?) {
        requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
    }

    // Games are fullscreen whatever window the port asks SDL for (Zelda64Recomp asks for a resizable one, so SDL
    // keeps Android's status and navigation bars). A swipe shows them briefly.
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
        }
    }

    private fun showLoading() {
        val title = intent.getStringExtra(EXTRA_TITLE) ?: ""
        val hint = TextView(this).apply {
            setTextColor(Color.argb(170, 255, 255, 255))
            textSize = 15f
            gravity = Gravity.CENTER
            setPadding(0, 24, 0, 0)
        }
        val view = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(Color.BLACK)
            isClickable = true // keep touches from reaching the game underneath
            addView(TextView(context).apply {
                text = title
                setTextColor(Color.WHITE)
                textSize = 26f
                gravity = Gravity.CENTER
            })
            addView(ProgressBar(context).apply { isIndeterminate = true; setPadding(0, 32, 0, 0) })
            addView(hint)
        }
        addContentView(view, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        loading = view
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed({
            if (loading != null) hint.text = "The first start takes longer while graphics are prepared for this device."
        }, SLOW_START_HINT_MS)
        handler.postDelayed({ hideLoading() }, LOADING_TIMEOUT_MS)
    }

    private fun hideLoading() {
        val view = loading ?: return
        loading = null
        view.animate().alpha(0f).setDuration(250).withEndAction { (view.parent as? ViewGroup)?.removeView(view) }.start()
    }

    override fun getArguments(): Array<String> =
        arrayOf(requireNotNull(intent.getStringExtra(EXTRA_LAUNCH_JSON)) { "no launch.json in the launch intent" })

    // The host owns Back (MODULE_SPEC §4.2): the game never sees it. Back opens the host's in-game menu
    // (docs/planning/MENU.md); quitting is one of its entries.
    //
    // Quick save states: hold L4 (the RP6's upper back button, reported as BUTTON_C) and press Y to save, X to load
    // (one slot for now). L4 itself and, while it's held, Y and X go to the shortcut, not the game.
    private var hotkeyHeld = false

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Select does nothing for now and never reaches the game (owner 2026-11-15): the N64 has no Select, so ports use
        // it only for their own menus (GoldenEye's options overlay, for one), which our menu replaces. It becomes a key
        // the player can bind once the Input screen exists. Our menu is on Back only.
        // Exception: a module whose layout binds Select (launch.json "select_to_game", from the manifest): SoH puts
        // N64 L there to match the button-glyph mod we pre-install.
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT && !selectToGame) return true
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            if (event.action == KeyEvent.ACTION_UP && menu == null) showMenu()
            return true
        }
        // Quick keys only while the save-states flag is on (post-alpha, Settings → Experimental); otherwise L4 passes on.
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_C && saveStatesOn) {
            hotkeyHeld = event.action == KeyEvent.ACTION_DOWN
            return true
        }
        if (hotkeyHeld && menu == null &&
            (event.keyCode == KeyEvent.KEYCODE_BUTTON_Y || event.keyCode == KeyEvent.KEYCODE_BUTTON_X)) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                if (event.keyCode == KeyEvent.KEYCODE_BUTTON_Y) quickSave() else quickLoad()
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    private fun quickSave() {
        val actions = gameActions()
        val save = actions.saveState ?: return notice("This game doesn't support save states yet")
        save()
        notice("State saved")
    }

    private fun quickLoad() {
        val actions = gameActions()
        val load = actions.loadState ?: return notice("This game doesn't support save states yet")
        if (!actions.hasState()) return notice("No state saved yet")
        load()
        notice("State loaded")
    }

    private fun notice(text: String) {
        android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show()
    }

    /** The in-game menu (GameMenu): pauses the module while it's open, resumes it when it closes. */
    private fun showMenu() {
        val title = intent.getStringExtra(EXTRA_TITLE) ?: ""
        val settingsFile = launchJson?.optString("settings_file")?.takeIf { it.isNotEmpty() }?.let { java.io.File(it) }
        val options = launchJson?.optJSONArray("menu_options") ?: org.json.JSONArray()
        // The in-game menu reopens on the tab and row it was closed on (per game; Settings → General → Remember
        // position). Written by the game process to its own file (UiState game = true).
        val uiState = app.portlauncher.util.UiState(filesDir, game = true)
        val position = uiState.position("game:${moduleId()}")
        val gameMenu = GameMenu(
            context = this,
            title = title,
            initialTab = position.first,
            initialRow = position.second,
            onPosition = { tab, row -> uiState.putPosition("game:${moduleId()}", tab, row) },
            scope = MenuScope("game", title),
            loadOptions = { options },
            profiles = app.portlauncher.library.SettingsProfile.listFromJson(launchJson?.optJSONArray("profiles")),
            storeSetting = { _, key, value ->
                // Saved for this game (passed in at the next launch) and applied now.
                settingsFile?.let { app.portlauncher.library.GameSettings(it).set(key, value) }
                CoreBridge.callModuleString(moduleLib, "plm_apply_settings",
                    org.json.JSONObject().put(key, value).toString())
            },
            resetSetting = { _, key, fallbackValue ->
                // Reset = drop the game's own choice (GameSettings.remove); the module gets the option's default
                // now, applied live like any change.
                settingsFile?.let { app.portlauncher.library.GameSettings(it).remove(key) }
                CoreBridge.callModuleString(moduleLib, "plm_apply_settings",
                    org.json.JSONObject().put(key, fallbackValue).toString())
            },
            gameActions = gameActions(),
            // The same Mods tab as the game page (owner 2026-11-15); mods load at start, so changes offer a restart.
            mods = ModsPage.forSpec(this, moduleId(), modsSpec, onRestart = { reset() },
                // Built-in mods switch live: their key goes to the module like a changed setting.
                applySetting = { key, value ->
                    CoreBridge.callModuleString(moduleLib, "plm_apply_settings", org.json.JSONObject().put(key, value).toString())
                },
                pickFile = ::pickFile),
            onClose = {
                menu = null
                hideSystemBars()
                CoreBridge.callModule(moduleLib, "plm_resume")
            },
        )
        menu = gameMenu
        CoreBridge.callModule(moduleLib, "plm_pause")
        gameMenu.show()
    }

    // Save states (docs/planning/SAVE_STATES.md): one slot per session for now, offered when the module exports them.
    private var stateSaved = false

    /** Save states are feature-flagged (post-alpha; Settings → Experimental). Read once per game launch. */
    private val saveStatesOn by lazy { app.portlauncher.util.AppFlags.isOn(filesDir, app.portlauncher.util.AppFlags.SAVE_STATES) }

    private fun gameActions(): GameActions {
        val library = moduleLib
        if (!saveStatesOn || !CoreBridge.hasModuleFunction(library, "plm_save_state")) return GameActions(onQuit = { quit() }, onReset = { reset() })
        return GameActions(
            onQuit = { quit() },
            onReset = { reset() },
            saveState = { CoreBridge.callModuleString(library, "plm_save_state", "0"); stateSaved = true },
            loadState = { CoreBridge.callModuleString(library, "plm_load_state", "0") },
            hasState = { stateSaved },
        )
    }

    private val modsSpec by lazy {
        app.portlauncher.mods.ModsSpec.fromJson(runCatching { org.json.JSONObject(intent.getStringExtra(EXTRA_MODS_SPEC)!!) }.getOrNull())
    }

    /** "Add a mod from a file" in game: the system file picker (this activity's result), then [callback]. */
    private var pickCallback: ((android.net.Uri?) -> Unit)? = null

    private fun pickFile(callback: (android.net.Uri?) -> Unit) {
        pickCallback = callback
        runCatching {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), REQUEST_PICK_MOD)
        }.onFailure { pickCallback = null; callback(null) }
    }

    @Deprecated("Activity result API isn't available on SDL's plain Activity")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQUEST_PICK_MOD) {
            val callback = pickCallback
            pickCallback = null
            callback?.invoke(if (resultCode == RESULT_OK) data?.data else null)
            return
        }
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
    }

    private val selectToGame by lazy { launchJson?.optBoolean("select_to_game", false) == true }

    /**
     * Brings the launch.json's "settings" and "menu_options" up to date with the game's settings file, before SDL reads
     * the launch.json (in super.onCreate) and before [launchJson] is first read.
     * Why (bug found 2026-11-16): Reset, and the restart after installing a mod in game, start this activity again with
     * the original intent, so the launch.json the launcher wrote at the first launch. Options changed in our menu since
     * then were saved to the settings file but not to that launch.json: the restarted game was handed the old values
     * and our menu showed them (SoH kept the changes only because it saves its own CVars). A fresh launch from the
     * library writes a new launch.json and needs nothing, but refreshing is harmless there.
     */
    private fun refreshLaunchSettings() {
        val path = intent.getStringExtra(EXTRA_LAUNCH_JSON) ?: return
        runCatching {
            val file = java.io.File(path)
            val launch = org.json.JSONObject(file.readText())
            val settingsPath = launch.optString("settings_file").takeIf { it.isNotEmpty() } ?: return
            val menu = launch.optJSONArray("menu_options") ?: return
            val user = app.portlauncher.library.GameSettings(java.io.File(settingsPath)).load()
            val settings = launch.optJSONObject("settings") ?: org.json.JSONObject().also { launch.put("settings", it) }
            for (i in 0 until menu.length()) {
                val option = menu.getJSONObject(i)
                val key = option.optString("key")
                if (user.has(key)) {
                    val value = user.getString(key)
                    option.put("value", value).put("source", "game").put("set_by_user", true)
                    settings.put(key, value)
                } else if (option.optString("source") == "game") {
                    // Reset in game since the first launch: back to the default, which the module must be told about.
                    val default = option.optString("default")
                    option.put("value", default).put("source", "default").put("set_by_user", false)
                    settings.put(key, default)
                }
            }
            // Built-in mods switched in game since the first launch (ModsSpec.builtin, BuiltinModState).
            val spec = app.portlauncher.mods.ModsSpec.fromJson(runCatching { org.json.JSONObject(intent.getStringExtra(EXTRA_MODS_SPEC)!!) }.getOrNull())
            app.portlauncher.mods.BuiltinModState(filesDir, moduleId()).settings(spec.builtin).forEach { (k, v) -> settings.put(k, v) }
            file.writeTextAtomic(launch.toString(2))
        }.onFailure { android.util.Log.w("plm-host", "couldn't refresh the launch settings", it) }
    }

    /** The launch's settings file and menu options (launch.json "settings_file", "menu_options"). */
    private val launchJson by lazy {
        runCatching { org.json.JSONObject(java.io.File(intent.getStringExtra(EXTRA_LAUNCH_JSON)!!).readText()) }.getOrNull()
    }

    /**
     * Reset (owner 2026-11-15): like a console's reset button, progress since the last in-game save is lost. A new
     * process with the same launch intent (RelaunchActivity explains why it goes through the main process); this one
     * ends at once instead of the graceful quit, because the module's state doesn't matter any more.
     */
    private fun reset() {
        startActivity(Intent(this, RelaunchActivity::class.java)
            .putExtra(RelaunchActivity.EXTRA_GAME_INTENT, Intent(intent)))
        finish()
        Process.killProcess(Process.myPid())
    }

    private fun quit() {
        if (!CoreBridge.requestModuleQuit(moduleLib)) {
            finish()
            return
        }
        // Normally plm_main returns and SDL finishes the activity; don't hang if the module never does.
        Handler(Looper.getMainLooper()).postDelayed({ if (!isFinishing) finish() }, QUIT_TIMEOUT_MS)
    }

    override fun onDestroy() {
        super.onDestroy()
        // A module runs once per process (MODULE_SPEC §4): end the process so the next launch starts clean.
        if (isFinishing) {
            Process.killProcess(Process.myPid())
        }
    }
}
