package app.portlauncher.host

import android.app.Dialog
import app.portlauncher.util.showTracked
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import app.portlauncher.library.MenuTab
import org.json.JSONArray

/**
 * Whose settings the page shows: one game's module (port). There is no "All games" layer any more (owner
 * 2026-11-15): settings are per game, plus the app's own settings (ROM sources), which take no scope.
 */
data class MenuScope(val id: String, val label: String)

/** In-game only: the main page's actions. Save states are offered when the module supports them. */
class GameActions(
    val onQuit: () -> Unit,
    /** Restart the game from power-on (Sdl2HostActivity.reset). Null hides the entry. */
    val onReset: (() -> Unit)? = null,
    val saveState: (() -> Unit)? = null,
    val loadState: (() -> Unit)? = null,
    val hasState: () -> Boolean = { false },
)

/**
 * An extra entry a caller puts on a tab for one scope (home screen only so far): "Mods and texture packs" on a game's
 * Mods tab, "Open <port>'s own launcher" on its Gameplay tab. Why this exists (2026-11-13): the library's per-game
 * Options button was removed (owner request), and these two actions lived in its dialog; the settings page is their
 * new home until the game page from the library rebuild (ROADMAP) exists. [run] is called after the menu closes.
 */
class MenuAction(val tab: String, val label: String, val detail: String, val run: () -> Unit)

/**
 * The home screen's "ROM sources" tab (owner 2026-11-13: the search folders are a settings page; the home screen's
 * Add folder button is gone). [folders] are (tree URI, display label), read again on every refresh; [onAdd] opens the
 * system folder picker (the menu stays open; the caller calls GameMenu.refresh() when the list changes); [onRemove]
 * runs after the player confirms. RomM shows as a placeholder entry until RommSource is implemented.
 */
/** App settings only: on/off switches for unfinished features (util/AppFlags). [get]/[set] read and write the flag. */
class FlagToggle(val label: String, val detail: String, val get: () -> Boolean, val set: (Boolean) -> Unit)

class SourcesPage(
    val folders: () -> List<Pair<String, String>>,
    val onAdd: () -> Unit,
    val onRemove: (uri: String) -> Unit,
    /** Scans every folder again (and refreshes the module server's offer); moved here from the library (2026-11-16). */
    val onRescan: () -> Unit = {},
    val rommConfigured: () -> Boolean = { false },
)

/**
 * The settings page (docs/planning/MENU.md "Menu pages"): full-screen and tabbed, shared by the in-game menu (Back)
 * and the home screen's settings. Layout follows the tabbed menus players know from other ports (recomp frontends,
 * Ship of Harkinian-style ports): a tab strip on top (L1/R1), the tab's entries on the left, a description of the
 * focused entry on the right, button hints at the bottom.
 *
 * Three uses (owner 2026-11-15: "per-game settings and app settings", and selecting a game opens its page instead of
 * launching it):
 *  - in-game (Back): [gameActions] set; the main page has Resume, save states, Reset, Quit;
 *  - the library's game page: [scope] set and [onLaunch] set; the main page leads with "Launch game", focused, so A
 *    on a game row then A again starts it (two presses instead of one, in exchange for every setting being one tab
 *    away before playing);
 *  - the app's settings (the library's Settings button): [scope] null, only the app pages (ROM sources).
 *
 * Options are GameSettings.describe() objects from [loadOptions] (the port's own options and our per-game tweaks,
 * with their tab, value and source: game / default). A change goes to [storeSetting]: the in-game menu saves it for
 * the game and applies it live (plm_apply_settings); the game page only saves it (applied at the next launch).
 */
class GameMenu(
    private val context: Context,
    private val title: String,
    /** The game whose settings this page shows; null for the app's own settings. */
    private val scope: MenuScope?,
    private val loadOptions: (MenuScope) -> JSONArray,
    private val storeSetting: (MenuScope, key: String, value: String) -> Unit,
    /** The port's settings profiles (SettingsProfile), offered at the top of the Settings tab. */
    private val profiles: List<app.portlauncher.library.SettingsProfile> = emptyList(),
    /**
     * Reset one setting (X, or the ↺ at the end of its row; owner request 2026-11-13). The caller drops the
     * choice (GameSettings.remove) and, in-game, applies [fallbackValue] (the option's default) live. The menu
     * updates the row itself without reloading.
     */
    private val resetSetting: (MenuScope, key: String, fallbackValue: String) -> Unit = { _, _, _ -> },
    /** Per-scope extra entries (see MenuAction). Empty in-game: the Game tab covers what the game needs there. */
    private val scopeActions: (MenuScope) -> List<MenuAction> = { emptyList() },
    /** Home screen only: the ROM sources tab (null in-game, where it has no business). */
    private val sources: SourcesPage? = null,
    /** App settings only: the Experimental tab's feature flags (empty hides the tab). */
    private val experimental: List<FlagToggle> = emptyList(),
    /** The Mods tab's content (ModsPage), on the game page and in game; null shows "coming soon". */
    private val mods: ModsPage? = null,
    /** App settings only: the General tab's switches (empty hides the tab). */
    private val general: List<FlagToggle> = emptyList(),
    /** Where to open (UiState, "Remember position"): tab index and row index; out-of-range values fall back to 0. */
    private val initialTab: Int = 0,
    private val initialRow: Int = 0,
    /** Called on close with the tab and row the player was on, so the next opening can return there. */
    private val onPosition: (tab: Int, row: Int) -> Unit = { _, _ -> },
    private val gameActions: GameActions? = null,
    /** Game page only: starts the game (the main page's first, focused entry). */
    private val onLaunch: (() -> Unit)? = null,
    /** Game page only, when the game can't start yet (no ROM): why, shown on a disabled "Launch game" entry. */
    private val launchBlocked: String? = null,
    private val onClose: () -> Unit,
) {
    /**
     * One tab. [id] is MenuTab.GAME / SOURCES for the fixed pages, else "<group>\u0000<label>" for a settings tab.
     * [group] is the port's top menu (libultraship's Settings / Enhancements), shown over its first tab in the strip.
     */
    private data class Tab(val id: String, val label: String, val group: String = "")

    /**
     * The tabs for the current scope (owner 2026-11-15): the main page, then one tab per tab of the port's own menu,
     * with the port's names, in the port's order (the option list comes in that order: GameSettings.resolve puts the
     * schema first, and the schema generators keep the menu order), then ROM sources on the app's settings. Our own
     * per-game tweaks join the first tab of the same name, or get their own after the port's (MenuTab.label).
     */
    private var tabs: List<Tab> = emptyList()

    /**
     * The port's own menu categories (its tabs, with their names and order), listed on the Settings tab's landing page
     * (owner 2026-11-16: all settings in a submenu, not as top tabs; MENU.md two-level proposal, built as a drill-down
     * list rather than a sidebar to keep the one-list layout). [category] is the one open, null on the landing page.
     */
    private var categories: List<Tab> = emptyList()
    private var category: String? = null

    /** The profile picked on the landing page: an index into [profiles], or profiles.size for "Port defaults". */
    private var profileIndex = 0

    private fun buildTabs(): List<Tab> {
        val out = mutableListOf<Tab>()
        if (gameActions != null || scope != null) {
            // Owner 2026-11-15: the main page, then Input (game inputs to device inputs, rebinding), then Mods and
            // Achievements as tabs of their own, then the port's settings. The three are stubs ("coming soon") for now.
            out += Tab(MenuTab.GAME, "Game")
            out += Tab(MenuTab.INPUT, "Input")
            out += Tab(MenuTab.MODS, "Mods")
            out += Tab(MenuTab.ACHIEVEMENTS, "Achievements")
        }
        val options = scope?.let(loadOptions) ?: JSONArray()
        val cats = mutableListOf<Tab>()
        for (i in 0 until options.length()) {
            val id = tabIdOf(options.getJSONObject(i), cats)
            if (cats.none { it.id == id }) {
                val o = options.getJSONObject(i)
                cats += Tab(id, o.optString("tab_label", MenuTab.label(o.optString("tab"))), o.optString("tab_group"))
            }
        }
        categories = cats
        if (cats.isNotEmpty()) out += Tab(MenuTab.SETTINGS, "Settings")
        if (sources != null) out += Tab(MenuTab.SOURCES, "ROM sources")
        if (general.isNotEmpty()) out += Tab(MenuTab.GENERAL, "General")
        if (experimental.isNotEmpty()) out += Tab(MenuTab.EXPERIMENTAL, "Experimental")
        return out
    }

    /** The tab an option belongs to; an option without a group joins an existing tab of the same name. */
    private fun tabIdOf(option: org.json.JSONObject, existing: List<Tab>): String {
        val label = option.optString("tab_label", MenuTab.label(option.optString("tab")))
        val group = option.optString("tab_group")
        if (group.isEmpty()) existing.firstOrNull { it.label.equals(label, ignoreCase = true) && it.id.contains('\u0000') }
            ?.let { return it.id }
        return "$group\u0000$label"
    }

    private val density = context.resources.displayMetrics.density
    private fun px(dp: Int) = (dp * density).toInt()

    private val dialog = Dialog(context, android.R.style.Theme_DeviceDefault_NoActionBar_Fullscreen)
    private val tabViews = mutableListOf<TextView>()
    private val tabStrip = android.widget.HorizontalScrollView(context)
    private val tabRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
    private val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private val scroller = ScrollView(context).apply { addView(list); isFillViewport = true }
    private val detailTitle = text("", 22f, Color.WHITE, bold = true)
    private val detailBody = text("", 16f, Color.argb(200, 255, 255, 255))
    private val detailState = text("", 14f, Color.argb(150, 255, 255, 255))
    private var current = 0

    /** One entry on a tab: a label, a description for the side panel, and actions for A and left/right. */
    private inner class Entry(
        val view: LinearLayout,
        val describe: () -> Triple<String, String, String>,
        val activate: () -> Unit,
        val adjust: ((Int) -> Unit)? = null,
        /** X: back to the inherited value (settings only). */
        val reset: (() -> Unit)? = null,
        /** Y: the entry's options (mods: order, remove). */
        val options: (() -> Unit)? = null,
    )

    private val entries = mutableMapOf<View, Entry>()

    init {
        // Background mod work (downloads, moves) redraws the Mods tab when it's the one showing, keeping the focus.
        mods?.onUpdate = { if (dialog.isShowing && tabs.getOrNull(current)?.id == MenuTab.MODS) refreshKeepingFocus() }
    }

    /** Redraws the current tab with the focus on the same row (by position), e.g. after toggling a mod. */
    private fun refreshKeepingFocus() {
        val rows = (0 until list.childCount).map { list.getChildAt(it) }.filter { it in entries }
        val focusedIndex = rows.indexOf(dialog.currentFocus)
        selectTab(current, focusedIndex)
    }

    fun show() {
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            // In game the paused picture shows through a little; over the library the page is solid, so the list
            // behind it doesn't read through (owner 2026-11-15, menu polish).
            setBackgroundColor(if (gameActions != null) Color.argb(242, 14, 14, 18) else Color.rgb(14, 14, 18))
            setPadding(px(32), px(20), px(32), px(16))
        }
        root.addView(header())
        root.addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(scroller, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 3f))
            addView(detailPanel(), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 2f).apply {
                leftMargin = px(24)
            })
        }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f).apply { topMargin = px(16) })
        root.addView(hintsView.apply {
            gravity = Gravity.CENTER
            setPadding(0, px(12), 0, 0)
        })

        dialog.setContentView(root)
        dialog.window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS)
        }
        dialog.setOnKeyListener { _, keyCode, event -> onKey(keyCode, event) }
        dialog.setOnDismissListener {
            val rows = (0 until list.childCount).map { list.getChildAt(it) }.filter { it in entries }
            onPosition(current, rows.indexOf(dialog.currentFocus).coerceAtLeast(0))
            onClose()
        }
        rebuildTabs()
        selectTab(initialTab.coerceIn(0, tabs.size - 1), initialRow)
        app.portlauncher.util.OpenDialogs.track(dialog).show()
    }

    fun dismiss() = dialog.dismiss()

    /** Rebuilds the current tab (e.g. after a folder was added or removed while the menu is open). */
    fun refresh() {
        if (dialog.isShowing) selectTab(current)
    }

    // ----------------------------------------------------------------------------------------------- layout ---

    private fun header(): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        addView(text(title, 26f, Color.WHITE, bold = true))
        // Scrolls sideways on narrow screens; the selected tab is kept in view (selectTab).
        addView(tabStrip.apply {
            isHorizontalScrollBarEnabled = false
            addView(tabRow.apply { setPadding(0, px(12), 0, 0) })
        })
    }

    /** Fills the tab strip for the current scope. A port's top menu shows as a small label before its first tab. */
    private fun rebuildTabs() {
        tabs = buildTabs()
        lastShownTab = -1
        tabRow.removeAllViews()
        tabViews.clear()
        tabRow.addView(text("L1", 14f, Color.argb(140, 255, 255, 255)).apply { setPadding(0, 0, px(12), 0) })
        var group = ""
        tabs.forEachIndexed { index, tab ->
            if (tab.group != group && tab.group.isNotEmpty()) {
                tabRow.addView(text(tab.group.uppercase(), 12f, Color.argb(130, 255, 255, 255)).apply {
                    setPadding(px(if (index == 0) 0 else 20), 0, px(4), 0)
                })
            }
            group = tab.group
            val view = text(tab.label, 18f, Color.WHITE).apply {
                setPadding(px(16), px(8), px(16), px(8))
                setOnClickListener { category = null; selectTab(index) }
            }
            tabViews += view
            tabRow.addView(view)
        }
        tabRow.addView(text("R1", 14f, Color.argb(140, 255, 255, 255)).apply { setPadding(px(12), 0, 0, 0) })
    }

    /**
     * The description panel on the right. Scrolls when the text overflows (owner 2026-11-15): by touch, or with L2 / R2
     * on a controller (the focus stays on the left list). Each new description starts at the top.
     */
    private val detailScroll = ScrollView(context)

    private fun detailPanel(): View = detailScroll.apply {
        background = GradientDrawable().apply {
            setColor(Color.argb(40, 255, 255, 255))
            cornerRadius = px(12).toFloat()
        }
        isFocusable = false
        isVerticalScrollBarEnabled = true
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(px(20), px(20), px(20), px(20))
            addView(detailTitle)
            addView(detailBody.apply { setPadding(0, px(10), 0, 0) })
            addView(detailState.apply { setPadding(0, px(14), 0, 0) })
        })
    }

    private var lastShownTab = -1

    /** The button hints follow the tab (menu polish 2026-11-15): only the keys that do something here. */
    private val hintsView by lazy { text("", 14f, Color.argb(140, 255, 255, 255)) }

    private fun updateHints(tabId: String) {
        val keys = mutableListOf("L1 / R1  Tabs")
        when {
            tabId == MenuTab.MODS && mods != null && !mods.browsing -> keys += listOf("A  On/off", "X  Remove", "Y  Order")
            tabId == MenuTab.MODS && mods != null -> keys += "A  Select"
            tabId == MenuTab.GENERAL || tabId == MenuTab.EXPERIMENTAL -> keys += "A or ◀ ▶  Switch"
            tabId == MenuTab.SETTINGS && category != null -> keys += listOf("◀ ▶  Change", "X  Reset")
            tabId == MenuTab.SETTINGS -> keys += listOf("◀ ▶  Profile", "A  Select")
            else -> keys += "A  Select"
        }
        keys += "L2 / R2  Scroll text"
        keys += when {
            tabId == MenuTab.SETTINGS && category != null -> "B  All settings"
            gameActions != null -> "B  Resume"
            else -> "B  Back"
        }
        hintsView.text = keys.joinToString("     ")
    }

    private fun selectTab(index: Int, keepFocus: Int = -1) {
        current = (index + tabs.size) % tabs.size
        tabViews.forEachIndexed { i, view ->
            view.background = if (i == current) GradientDrawable().apply {
                setColor(Color.argb(70, 255, 255, 255))
                cornerRadius = px(8).toFloat()
            } else null
            view.alpha = if (i == current) 1f else 0.6f
        }
        // Keep the tab row still (owner 2026-11-15: it bounced on every redraw): scroll only when the selected tab
        // changed and is (partly) out of view, by the least amount that shows it, without animation.
        if (current != lastShownTab) {
            lastShownTab = current
            tabViews[current].let { tab ->
                tabStrip.post {
                    val left = tab.left - px(48)
                    val right = tab.right + px(48) - tabStrip.width
                    when {
                        left < tabStrip.scrollX -> tabStrip.scrollTo(maxOf(0, left), 0)
                        right > tabStrip.scrollX -> tabStrip.scrollTo(right, 0)
                    }
                }
            }
        }
        list.removeAllViews()
        entries.clear()
        val tab = tabs[current]
        updateHints(tab.id)
        when (tab.id) {
            MenuTab.GAME -> {
                val actions = gameActions
                if (actions != null) addEntry(action("Resume", "Back to the game.") { dismiss() })
                // Game page: launching is the default action (first entry, so it gets the focus below).
                onLaunch?.let { launch ->
                    addEntry(action("Launch game", "Starts $title with the settings on these pages.") { dismiss(); launch() })
                }
                if (onLaunch == null && launchBlocked != null) addEntry(action("Launch game: ROM needed", launchBlocked) {})
                if (actions?.saveState != null && actions.loadState != null) {
                    addEntry(action("Save state", "Saves the game exactly as it is now. Kept until you quit the game.") {
                        actions.saveState.invoke()
                        dismiss()
                    })
                    addEntry(action("Load state", "Returns to the state you saved this session.") {
                        if (actions.hasState()) {
                            actions.loadState.invoke()
                            dismiss()
                        } else {
                            showDetail("Load state", "No state saved yet this session: use Save state first.", "")
                        }
                    })
                }
                actions?.onReset?.let { reset ->
                    addEntry(action("Reset", "Restarts the game from the beginning, like the console's reset button. Progress since your last save is lost.") {
                        android.app.AlertDialog.Builder(context, android.R.style.Theme_DeviceDefault_Dialog)
                            .setTitle("Reset the game?")
                            .setMessage("Progress since your last save will be lost.")
                            .setPositiveButton("Reset") { _, _ -> dismiss(); reset() }
                            .setNegativeButton("Cancel", null)
                            .showTracked()
                    })
                }
                val extras = scope?.let(scopeActions).orEmpty()
                extras.filter { it.tab == MenuTab.GAME }.forEach { extra ->
                    addEntry(action(extra.label, extra.detail) { dismiss(); extra.run() })
                }
                if (actions != null) addEntry(action("Quit to library", "Progress since your last save will be lost.") {
                    dismiss()
                    actions.onQuit()
                })
            }
            MenuTab.INPUT -> addEntry(action("Controls (coming soon)",
                "Which button does what: the game's actions mapped to this device's controls, with rebinding. Not available yet.") {})
            MenuTab.MODS -> {
                val page = mods
                if (page == null) {
                    addEntry(action("Mods and texture packs (coming soon)",
                        "Download, switch on and off, and order mods and texture packs for this game. Not available yet.") {})
                } else {
                    modsTab(page)
                }
            }
            MenuTab.ACHIEVEMENTS -> addEntry(action("Achievements (coming soon)",
                "This game's achievements and your progress. Not available yet.") {})
            MenuTab.GENERAL, MenuTab.EXPERIMENTAL -> (if (tab.id == MenuTab.GENERAL) general else experimental).forEach { flag ->
                lateinit var view: LinearLayout
                fun valueText() = if (flag.get()) "◀ On ▶" else "◀ Off ▶"
                fun toggle() {
                    flag.set(!flag.get())
                    view.findViewWithTag<TextView>("value")?.text = valueText()
                }
                view = row(flag.label, valueText())
                view.setOnClickListener { toggle() }
                addEntry(Entry(view, { Triple(flag.label, flag.detail, "Applies at the next game launch") }, ::toggle, { toggle() }))
            }
            MenuTab.SOURCES -> sources?.let { page ->
                // The same for every scope: ROM folders belong to the library, not to a game.
                for ((uri, label) in page.folders()) {
                    addEntry(action(label, "Scanned for N64 ROMs. Select to remove it from the library (the files stay where they are).") {
                        android.app.AlertDialog.Builder(context, android.R.style.Theme_DeviceDefault_Dialog)
                            .setTitle("Remove this folder?")
                            .setMessage("$label\n\nIts games leave the library. Nothing is deleted.")
                            .setPositiveButton("Remove") { _, _ -> page.onRemove(uri) }
                            .setNegativeButton("Cancel", null)
                            .showTracked()
                    })
                }
                addEntry(action("Add a folder", "Pick another folder with N64 ROMs (.z64, .v64, .n64 or .zip). Its subfolders are scanned too. The library rescans afterwards.") {
                    page.onAdd()
                })
                addEntry(action("Rescan the library", "Looks through every folder above again, subfolders included, for ROMs added or removed since the last scan, and checks the module server for new modules and updates.") {
                    dismiss()
                    page.onRescan()
                })
                // RomM placeholder (RommSource is a stub): visible so the page shows where the server will go.
                addEntry(action(
                    if (page.rommConfigured()) "RomM server" else "RomM server (coming soon)",
                    "Your own RomM server as a ROM source: its games show in the library with a RomM badge and download when you play. Not available yet.",
                ) {})
            }
            MenuTab.SETTINGS -> {
                val open = category
                if (open == null) settingsLanding() else {
                    addEntry(action("◀ All settings", "Back to the profiles and the list of categories.") { closeCategory() })
                    settingsFor(open)
                }
            }
        }
        if (entries.isEmpty()) {
            list.addView(text("Nothing to change here.", 16f, Color.argb(170, 255, 255, 255)).apply {
                setPadding(px(16), px(16), px(16), px(16))
            })
            showDetail(tab.label, "Nothing to change here.", "")
        } else {
            val rows = (0 until list.childCount).map { list.getChildAt(it) }.filter { it in entries }
            rows.getOrElse(keepFocus) { rows.first() }.requestFocus()
        }
        if (keepFocus < 0) scroller.scrollTo(0, 0)
    }

    /**
     * The Mods tab (owner 2026-11-15). Installed view: each mod's row toggles with A or left/right; Y (or the row's
     * long press) opens its options: move up/down in the load order (later mods win where two replace the same thing),
     * remove. "Get more mods" switches to the source's browse list, where A installs or updates. In game, a change
     * offers "Restart to apply" first (mods load when the game starts).
     */
    private fun modsTab(page: ModsPage) {
        val busyNote = page.status?.let { "\n\n$it" }.orEmpty()
        if (!page.browsing) {
            if (page.pendingRestart && page.onRestart != null) {
                addEntry(action("Restart to apply", "Mods load when the game starts: restart now to use your changes. Progress since your last save is lost.") {
                    dismiss(); page.onRestart.invoke()
                })
            }
            // Built-in mods (owner 2026-11-16): our features compiled into the game, switched on or off, never removed.
            page.builtin.forEach { m ->
                lateinit var view: LinearLayout
                fun valueText() = if (page.builtinOn(m)) "◀ On ▶" else "◀ Off ▶"
                val toggle = {
                    page.toggleBuiltin(m)
                    view.findViewWithTag<TextView>("value")?.text = valueText()
                }
                view = row(m.name, valueText())
                view.setOnClickListener { toggle() }
                addEntry(Entry(view, {
                    Triple(m.name, m.description + "\n\nBuilt into this game by the launcher. Off plays the original." +
                        (if (page.onRestart != null) " Applies right away." else ""), "A or ◀ ▶  on/off")
                }, toggle, { toggle() }, reset = { showDetail(m.name, "Built into this game: it can be switched off, not removed.", "") }))
            }
            page.installed.forEachIndexed { index, p ->
                lateinit var view: LinearLayout
                fun valueText() = if (p.enabled) "◀ On ▶" else "◀ Off ▶"
                val describe = {
                    Triple(p.displayName,
                        "Load order ${index + 1} of ${page.installed.size}: later mods win where two change the same thing." + busyNote,
                        "A or ◀ ▶  on/off     X  remove     Y  move up/down")
                }
                view = row(p.displayName, valueText())
                view.setOnClickListener { page.toggle(p) }
                view.setOnLongClickListener { modOptions(page, p); true }
                addEntry(Entry(view, describe, { page.toggle(p) }, { page.toggle(p) }, reset = { confirmRemove(page, p) }, options = { modOptions(page, p) }))
            }
            val curated = page.source is app.portlauncher.mods.CuratedSource
            if (page.hasBrowse) {
                addEntry(action(if (curated) "Recommended mods" else "Get more mods",
                    (if (curated) "Mods we recommend for this game, made by the community and credited to their authors. They download from each author's own release."
                     else "Browse ${page.source.label}'s mods for this game. They're made by the community.") + busyNote) {
                    page.browsing = true
                    if (page.catalogue == null) page.loadCatalogue() else selectTab(current)
                })
            }
            if (page.canAddFiles) {
                addEntry(action("Add a mod from a file…", "Install a mod you downloaded yourself (.o2r, .otr or .zip): pick it from this device's storage." + busyNote) {
                    page.addFromFile()
                })
            }
            if (page.spec.sources.isNotEmpty()) {
                addEntry(action("Where to get more mods", "Sites with mods for this game, opened in your web browser. Download a mod there, then come back and use \"Add a mod from a file\"." + busyNote) {
                    sourcesDialog(page)
                })
            }
            if (page.installed.isEmpty() && page.builtin.isEmpty() && !page.hasBrowse && page.spec.sources.isEmpty()) {
                addEntry(action("No mods for this game yet", "Nothing to install here yet." + busyNote) {})
            }
        } else if (page.detailOf != null) {
            modDetail(page, page.detailOf!!)
        } else {
            addEntry(action("◀ Installed mods (${page.installed.size})", "Back to your installed mods." + busyNote) {
                page.browsing = false; selectTab(current)
            })
            // Search, sort and category (owner 2026-11-15: the lists are huge). A search ranks by best match, so sort
            // and category apply to browsing only.
            if (page.source.searchable) addEntry(action(if (page.query.text.isEmpty()) "Search…" else "Search: ${page.query.text}",
                "Search ${page.source.label}'s mods for this game by name. Clear the text to browse again." + busyNote) {
                searchDialog(page)
            })
            if (page.query.text.isEmpty() && page.source.sorts.isNotEmpty()) {
                lateinit var sortView: LinearLayout
                sortView = row("Sort", "◀ ${page.sortLabel()} ▶")
                val sortStep = { d: Int -> if (!page.busy) page.cycleSort(d) }
                sortView.setOnClickListener { sortStep(1) }
                addEntry(Entry(sortView, { Triple("Sort", "How the list is ordered." + busyNote, "◀ ▶  change") }, { sortStep(1) }, sortStep))
                if (page.categories.isNotEmpty()) {
                    val catView = row("Category", "◀ ${page.categoryLabel()} ▶")
                    val catStep = { d: Int -> if (!page.busy) page.cycleCategory(d) }
                    catView.setOnClickListener { catStep(1) }
                    addEntry(Entry(catView, { Triple("Category", "Show one kind of mod only (models, textures…)." + busyNote, "◀ ▶  change") }, { catStep(1) }, catStep))
                }
            }
            val catalogue = page.catalogue
            if (catalogue == null) {
                addEntry(action(if (page.busy) "Loading…" else "Try again", (page.status ?: "Loading the list…")) {
                    if (!page.busy) page.loadCatalogue(refresh = true)
                })
            } else {
                if (catalogue.isEmpty()) addEntry(action("Nothing found", "No mods match. Try another search or category." + busyNote) {})
                catalogue.forEach { pkg ->
                    val mine = page.installedOf(pkg)
                    val state = when {
                        mine.isEmpty() -> ""
                        mine.any { it.version != pkg.version } -> "Update"
                        else -> "Installed"
                    }
                    val view = row(pkg.displayName, state.ifEmpty { null })
                    val describe = {
                        Triple(pkg.displayName,
                            listOf(pkg.description, if (pkg.owner.isNotEmpty()) "by ${pkg.owner}" else "",
                                if (pkg.downloads > 0) "${pkg.downloads} downloads" else "")
                                .filter { it.isNotEmpty() }.joinToString(" · ") + busyNote,
                            "A  details, size and install")
                    }
                    val open = { if (!page.busy) page.openDetails(pkg) }
                    view.setOnClickListener { open() }
                    addEntry(Entry(view, describe, open))
                }
                if (page.hasMore) addEntry(action(if (page.busy) "Loading…" else "Load more", "The next page of the list." + busyNote) {
                    if (!page.busy) page.loadMore()
                })
            }
        }
    }

    /**
     * A mod's detail view: one install entry per file with its size (some mods offer several, e.g. a full pack and a
     * lighter one), the full description in the scrollable panel on the right.
     */
    private fun modDetail(page: ModsPage, pkg: app.portlauncher.mods.RemotePackage) {
        val d = page.details
        val busyNote = page.status?.let { "\n\n$it" }.orEmpty()
        val about = buildString {
            if (d != null) {
                if (d.summary.isNotBlank()) append(d.summary.trim()).append("\n\n")
                val facts = listOfNotNull(
                    if (pkg.owner.isNotEmpty()) "by ${pkg.owner}" else null,
                    if (d.likes > 0) "${d.likes} likes" else null,
                    if (d.downloads > 0) "${d.downloads} downloads" else null,
                    if (d.updated > 0) "updated " + java.text.DateFormat.getDateInstance().format(java.util.Date(d.updated * 1000)) else null,
                )
                append(facts.joinToString(" · "))
                if (d.text.isNotBlank()) append("\n\n").append(d.text)
                if (d.license.isNotBlank()) append("\n\nLicence: ").append(d.license)
            } else {
                append(listOf(pkg.description, if (pkg.owner.isNotEmpty()) "by ${pkg.owner}" else "").filter { it.isNotEmpty() }.joinToString(" · "))
            }
            append(busyNote)
        }
        // The panel shows the mod (name, description) while the focus is on the back entry, the first one.
        val back = row("◀ Back to the list", null)
        back.setOnClickListener { page.closeDetails() }
        addEntry(Entry(back, { Triple(pkg.displayName, about, "L2 / R2  scroll the description") }, { page.closeDetails() }))
        page.source.webUrl(pkg)?.let { url ->
            val label = if (page.source is app.portlauncher.mods.CuratedSource) "Open the mod's page" else "Open on ${page.source.label.substringBefore(" (")}"
            addEntry(action(label, "Opens this mod's page in your web browser: pictures, changelog, comments.\n\n$url") {
                openInBrowser(label, url)
            })
        }
        val files = d?.files.orEmpty()
        if (d == null) {
            addEntry(action(if (page.busy) "Loading…" else "Install", about) { if (!page.busy) page.install(pkg) })
        } else if (files.none { it.supported }) {
            addEntry(action("Can't be installed here", "None of its files is a format the launcher can install (.o2r, .otr or .zip): " +
                files.joinToString { it.name } + busyNote) {})
        } else {
            // Each file is its own item: its own Install / Installed / Update state.
            val multi = files.count { it.supported } > 1
            files.forEach { f ->
                val mine = page.installedFile(pkg, f.name, multi)
                val label = when {
                    mine == null -> "Install "
                    mine.version != pkg.version -> "Update "
                    else -> "Installed: "
                } + f.name
                val value = sizeText(f.size)
                val view = row(label, value)
                val run = { if (f.supported && !page.busy) page.install(pkg, f.name, multi) }
                view.setOnClickListener { run() }
                addEntry(Entry(view, {
                    Triple(f.name, (if (f.supported) "${sizeText(f.size)} download" + (if (f.downloads > 0) " · ${f.downloads} downloads" else "") +
                        (if (mine != null) "\n\nInstalled already: A downloads it again." else "")
                    else "This format can't be installed here.") + busyNote, if (f.supported) "A  install" else "")
                }, run))
            }
        }
    }

    private fun openInBrowser(title: String, url: String) {
        runCatching {
            context.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse(url))
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
        }.onFailure { showDetail(title, "No web browser is installed to open $url", "") }
    }

    /** "Where to get more mods" (owner 2026-11-16): the game's known mod sites, one opens in the browser. */
    private fun sourcesDialog(page: ModsPage) {
        val sources = page.spec.sources
        android.app.AlertDialog.Builder(context, android.R.style.Theme_DeviceDefault_Dialog)
            .setTitle("Where to get more mods")
            .setItems(sources.map { it.first }.toTypedArray()) { _, which ->
                val (label, url) = sources[which]
                openInBrowser(label, url)
            }
            .setNegativeButton("Close", null)
            .showTracked()
    }

    private fun sizeText(bytes: Long): String = when {
        bytes >= 1_000_000_000 -> "%.1f GB".format(bytes / 1e9)
        bytes >= 1_000_000 -> "%.1f MB".format(bytes / 1e6)
        bytes >= 1_000 -> "${bytes / 1_000} KB"
        bytes > 0 -> "$bytes B"
        else -> "size unknown"
    }

    /** Text entry for the mod search (the on-screen keyboard on a handheld). */
    private fun searchDialog(page: ModsPage) {
        val input = android.widget.EditText(context).apply {
            setText(page.query.text)
            setSingleLine()
            imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH
        }
        val dialog = android.app.AlertDialog.Builder(context, android.R.style.Theme_DeviceDefault_Dialog)
            .setTitle("Search mods")
            .setView(input)
            .setPositiveButton("Search") { _, _ -> page.search(input.text.toString()) }
            .setNeutralButton("Clear") { _, _ -> page.search("") }
            .setNegativeButton("Cancel", null)
            .create()
        input.setOnEditorActionListener { _, _, _ -> page.search(input.text.toString()); dialog.dismiss(); true }
        app.portlauncher.util.OpenDialogs.track(dialog).show()
    }

    /** Opens a settings category from the landing page. */
    private fun openCategory(id: String) {
        category = id
        selectTab(current)
    }

    /** Back from a category to the landing page, with the focus on that category's row. */
    private fun closeCategory() {
        val was = category ?: return
        category = null
        val landingRows = 2  // Profile and Apply: every game has them
        selectTab(current, landingRows + categories.indexOfFirst { it.id == was }.coerceAtLeast(0))
    }

    /** What applying the picked profile would change: (option, new value) for each option whose value differs. */
    private fun profileChanges(options: JSONArray): List<Pair<org.json.JSONObject, String>> {
        val profile = profiles.getOrNull(profileIndex)
        return (0 until options.length()).map { options.getJSONObject(it) }.mapNotNull { o ->
            val key = o.getString("key")
            if (profile == null) {
                // Port defaults: every value the player chose goes back to the port's default.
                if (o.optString("source") == "game") o to o.optString("default") else null
            } else profile.values[key]?.takeIf { it != o.optString("value") }?.let { o to it }
        }
    }

    private fun profileLabel() = profiles.getOrNull(profileIndex)?.label ?: "Port defaults"

    /**
     * The Settings tab's landing page (owner 2026-11-16): pick a profile (left/right), then Apply, whose row says how
     * many settings will change; below, the port's categories, A opens one. Profiles only set the values they name
     * (other settings keep yours); "Port defaults" resets every value you chose. Nothing changes until Apply.
     * Every game has the picker (owner 2026-11-16), even one without profiles: then it offers only "Port defaults",
     * which doubles as the reset for everything you changed.
     */
    private fun settingsLanding() {
        val scope = scope ?: return
        run {
            profileIndex = profileIndex.coerceIn(0, profiles.size)
            fun options() = loadOptions(scope)
            lateinit var applyView: LinearLayout
            fun applyText(n: Int) = if (n == 0) "No changes" else "$n setting${if (n == 1) "" else "s"} will change"
            fun describeProfile(): Triple<String, String, String> {
                val changes = profileChanges(options())
                val what = profiles.getOrNull(profileIndex)?.detail
                    ?: "Puts every setting you changed back to ${title}'s own default."
                val list = changes.joinToString("\n") { (o, v) ->
                    // Range options (sliders) have no "values" list: show the number itself.
                    fun label(x: String) = o.optJSONArray("values")?.let { a ->
                        (0 until a.length()).map { a.getJSONObject(it) }.firstOrNull { it.getString("value") == x }?.getString("label")
                    } ?: x
                    "${o.getString("label")}: ${label(o.getString("value"))} → ${label(v)}"
                }
                return Triple(profileLabel(), what + if (list.isEmpty()) "" else "\n\n$list",
                    applyText(changes.size) + "  ·  ◀ ▶ picks a profile, Apply below applies it")
            }
            lateinit var profileView: LinearLayout
            // Only the two landing rows (picking a profile changes nothing else). Not named refresh(): it shadowed
            // GameMenu.refresh() in apply() below, so the categories' "N set by you" stayed stale after Apply until the
            // tab was redrawn (owner report 2026-11-16).
            fun updateLandingRows() {
                profileView.findViewWithTag<TextView>("value")?.text = "◀ ${profileLabel()} ▶"
                applyView.findViewWithTag<TextView>("value")?.text = applyText(profileChanges(options()).size)
            }
            fun step(d: Int) {
                profileIndex = ((profileIndex + d) % (profiles.size + 1) + profiles.size + 1) % (profiles.size + 1)
                updateLandingRows()
                describeProfile().let { showDetail(it.first, it.second, it.third) }
            }
            profileView = row("Profile", "◀ ${profileLabel()} ▶")
            profileView.setOnClickListener { step(1) }
            addEntry(Entry(profileView, ::describeProfile, { step(1) }, ::step))
            fun apply() {
                val changes = profileChanges(options())
                if (changes.isEmpty()) {
                    showDetail("Apply ${profileLabel()}", "Nothing to change: your settings already match.", "")
                    return
                }
                for ((o, v) in changes) {
                    val key = o.getString("key")
                    if (profiles.getOrNull(profileIndex) == null) {
                        o.put("value", v).put("source", "default")
                        resetSetting(scope, key, v)
                    } else {
                        o.put("value", v).put("source", "game")
                        storeSetting(scope, key, v)
                    }
                }
                // Redraw the whole tab, so the categories' "N set by you" counts follow; focus stays on Apply (row 1).
                selectTab(current, 1)
                showDetail("Applied ${profileLabel()}", "${changes.size} setting${if (changes.size == 1) "" else "s"} changed." +
                    if (gameActions != null) " Some take effect only after a restart or a new area." else "", "")
            }
            applyView = row("Apply", applyText(profileChanges(options()).size))
            applyView.setOnClickListener { apply() }
            addEntry(Entry(applyView, {
                val n = profileChanges(options()).size
                Triple("Apply ${profileLabel()}",
                    if (n == 0) "Nothing to change: your settings already match ${profileLabel()}."
                    else "Changes ${applyText(n).substringBefore(" will")} to ${profileLabel()}'s values. Every other setting stays as it is. Each can still be changed one by one afterwards.",
                    applyText(n))
            }, ::apply))
        }
        // The port's categories, under its top menu names (libultraship's Settings / Enhancements) as headers.
        val options = loadOptions(scope)
        var group = ""
        for (cat in categories) {
            if (cat.group.isNotEmpty() && cat.group != group) {
                list.addView(text(cat.group, 14f, Color.argb(160, 255, 255, 255), bold = true).apply {
                    setPadding(px(16), px(18), px(16), px(4))
                })
            }
            group = cat.group
            val inCat = (0 until options.length()).map { options.getJSONObject(it) }.filter { tabIdOf(it, categories) == cat.id }
            val mine = inCat.count { it.optString("source") == "game" }
            val view = row(cat.label, if (mine > 0) "$mine set by you  ›" else "›")
            view.setOnClickListener { openCategory(cat.id) }
            addEntry(Entry(view, {
                Triple(cat.label, "${inCat.size} settings" + (if (cat.group.isNotEmpty()) " in ${cat.group}" else "") +
                    (if (mine > 0) ", $mine set by you." else "."), "A  open")
            }, { openCategory(cat.id) }))
        }
    }

    /** B on the Mods tab: one level back. False when there's nothing to step back from (the menu then closes). */
    private fun stepBackInMods(): Boolean {
        val page = mods ?: return false
        if (tabs.getOrNull(current)?.id != MenuTab.MODS) return false
        return when {
            page.detailOf != null -> { page.closeDetails(); true }
            page.browsing -> { page.browsing = false; selectTab(current); true }
            else -> false
        }
    }

    /** A mod's options: load order and removal. */
    private fun modOptions(page: ModsPage, p: app.portlauncher.mods.InstalledPackage) {
        val choices = arrayOf("Move up (loads earlier)", "Move down (loads later, wins conflicts)", "Remove")
        android.app.AlertDialog.Builder(context, android.R.style.Theme_DeviceDefault_Dialog)
            .setTitle(p.displayName)
            .setItems(choices) { _, which ->
                when (which) {
                    0 -> page.move(p, -1)
                    1 -> page.move(p, 1)
                    2 -> confirmRemove(page, p)
                }
            }
            .showTracked()
    }

    /** X on an installed mod (owner 2026-11-15: removing should be easy): one confirmation, then it's gone. */
    private fun confirmRemove(page: ModsPage, p: app.portlauncher.mods.InstalledPackage) {
        android.app.AlertDialog.Builder(context, android.R.style.Theme_DeviceDefault_Dialog)
            .setTitle("Remove ${p.displayName}?")
            .setMessage("Its files are deleted from this device. You can download it again any time.")
            .setPositiveButton("Remove") { _, _ -> page.remove(p) }
            .setNegativeButton("Cancel", null)
            .showTracked()
    }

    private fun addEntry(entry: Entry) {
        entries[entry.view] = entry
        entry.view.setOnFocusChangeListener { _, focused -> if (focused) entry.describe().let { showDetail(it.first, it.second, it.third) } }
        list.addView(entry.view)
    }

    private fun showDetail(title: String, body: String, state: String) {
        detailTitle.text = title
        detailBody.text = body
        detailBody.visibility = if (body.isBlank()) View.GONE else View.VISIBLE   // no gap under the title
        detailState.text = state
        detailState.visibility = if (state.isEmpty()) View.GONE else View.VISIBLE
        detailScroll.post { detailScroll.scrollTo(0, 0) }
    }

    // ---------------------------------------------------------------------------------------------- entries ---

    private fun row(label: String, value: String?, onReset: (() -> Unit)? = null): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        isFocusable = true
        isFocusableInTouchMode = true
        isClickable = true
        setPadding(px(16), px(14), px(16), px(14))
        background = StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_focused), GradientDrawable().apply {
                setColor(Color.argb(60, 255, 255, 255))
                setStroke(px(2), Color.WHITE)
                cornerRadius = px(10).toFloat()
            })
            addState(intArrayOf(), ColorDrawable(Color.TRANSPARENT))
        }
        addView(text(label, 19f, Color.WHITE), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (value != null) addView(text(value, 19f, Color.argb(220, 255, 255, 255)).apply { tag = "value" })
        if (onReset != null) {
            // The reset button for touch. Deliberately NOT focusable: left/right on a row change its value (onKey), so
            // a second focus target inside the row would be unreachable with the D-pad anyway; controllers use X.
            // Dimmed (tag "reset" + alpha, set in settingsFor) when the row has no choice of its own to reset.
            addView(text("↺", 22f, Color.WHITE).apply {
                tag = "reset"
                isFocusable = false
                setPadding(px(18), 0, px(4), 0)
                setOnClickListener { onReset() }
            })
        }
    }

    private fun action(label: String, detail: String, run: () -> Unit): Entry {
        val view = row(label, null)
        view.setOnClickListener { run() }
        return Entry(view, { Triple(label, detail, "") }, run)
    }

    /**
     * The tab's settings for the current scope, added to the list: A or right steps forward through the values, left
     * back. The port's section headers (the separator text inside its tab) go in between, as in the original menu.
     */
    private fun settingsFor(tabId: String) {
        val current = scope ?: return
        val options = loadOptions(current)
        var section = ""
        for (i in 0 until options.length()) {
            val option = options.getJSONObject(i)
            if (tabIdOf(option, categories) != tabId) continue
            val header = option.optString("section")
            if (header.isNotEmpty() && header != section) {
                list.addView(text(header, 14f, Color.argb(160, 255, 255, 255), bold = true).apply {
                    setPadding(px(16), px(if (list.childCount == 0) 4 else 18), px(16), px(4))
                })
            }
            section = header
            val values = option.getJSONArray("values")
            fun labelOf(v: String) = (0 until values.length()).map { values.getJSONObject(it) }
                .firstOrNull { it.getString("value") == v }?.getString("label") ?: v
            // Long value lists (sliders stepped in 5 %) read as a range, not every step (menu polish 2026-11-15).
            fun valuesText() = if (values.length() > 8)
                "${values.getJSONObject(0).getString("label")} to ${values.getJSONObject(values.length() - 1).getString("label")}"
            else (0 until values.length()).joinToString("  ·  ") { values.getJSONObject(it).getString("label") }
            // Only a value the player chose can be reset; the default already is what a reset gives.
            fun resettable() = option.optString("source") == "game"
            lateinit var view: LinearLayout
            fun refreshReset() { view.findViewWithTag<TextView>("reset")?.alpha = if (resettable()) 1f else 0.25f }
            // "game" is the per-module layer (the name predates the per-module change, 2026-11-13).
            fun sourceText() = if (option.optString("source") == "game") "Set by you" else "Default"
            fun describe() = Triple(option.getString("label"), option.optString("detail"),
                sourceText() + (if (resettable()) "  ·  X resets" else "") + "\n" + valuesText())
            fun reset() {
                if (!resettable()) return
                val value = option.optString("default")
                option.put("value", value).put("source", "default")
                resetSetting(current, option.getString("key"), value)
                view.findViewWithTag<TextView>("value")?.text = "◀ ${labelOf(value)} ▶"
                refreshReset()
                describe().let { showDetail(it.first, it.second, it.third) }
            }
            view = row(option.getString("label"), "◀ ${labelOf(option.getString("value"))} ▶", ::reset)
            refreshReset()
            fun step(direction: Int) {
                val now = option.getString("value")
                val index = (0 until values.length()).indexOfFirst { values.getJSONObject(it).getString("value") == now }
                val next = values.getJSONObject(((index + direction) % values.length() + values.length()) % values.length())
                    .getString("value")
                option.put("value", next).put("source", "game")
                storeSetting(current, option.getString("key"), next)
                view.findViewWithTag<TextView>("value")?.text = "◀ ${labelOf(next)} ▶"
                refreshReset()
                describe().let { showDetail(it.first, it.second, it.third) }
            }
            view.setOnClickListener { step(1) }
            addEntry(Entry(view, ::describe, { step(1) }, ::step, ::reset))
        }
    }

    // ------------------------------------------------------------------------------------------------ input ---

    private fun onKey(keyCode: Int, event: KeyEvent): Boolean {
        val up = event.action == KeyEvent.ACTION_UP
        val focused = dialog.currentFocus?.let { entries[it] }
        return when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_X -> { if (up) focused?.reset?.invoke(); true }
            KeyEvent.KEYCODE_BUTTON_Y -> { if (up) focused?.options?.invoke(); true }
            KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2 -> {
                // The description panel scrolls; held triggers repeat (key repeat events come as ACTION_DOWN).
                if (!up) detailScroll.smoothScrollBy(0, (if (keyCode == KeyEvent.KEYCODE_BUTTON_R2) 1 else -1) * px(160))
                true
            }
            KeyEvent.KEYCODE_BUTTON_L1 -> { if (up) { category = null; selectTab(current - 1) }; true }
            KeyEvent.KEYCODE_BUTTON_R1 -> { if (up) { category = null; selectTab(current + 1) }; true }
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT -> {
                // Left/right change the focused setting; they never move focus (that's up/down and L1/R1).
                if (!up) focused?.adjust?.invoke(if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT) 1 else -1)
                true
            }
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> {
                if (up) focused?.activate?.invoke()
                true
            }
            KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BACK -> {
                // Inside the Mods tab, B steps back first (owner 2026-11-15): a mod's details -> the browse list ->
                // the installed mods; only from there does it close the menu.
                if (up && !stepBackInMods()) {
                    if (category != null && tabs.getOrNull(current)?.id == MenuTab.SETTINGS) closeCategory() else dismiss()
                }
                true
            }
            else -> false
        }
    }

    private fun text(value: String, size: Float, color: Int, bold: Boolean = false) = TextView(context).apply {
        text = value
        textSize = size
        setTextColor(color)
        if (bold) typeface = Typeface.DEFAULT_BOLD
    }
}
