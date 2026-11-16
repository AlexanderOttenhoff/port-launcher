package app.portlauncher.util

import android.app.AlertDialog
import android.app.Dialog
import java.lang.ref.WeakReference

/**
 * Every menu window the launcher opens (game pages, app Settings, their confirmations and pickers), so they can all be
 * closed at once. Owner 2026-11-16: opening the app from the home-screen icon lands on the library with no menu open;
 * switching back through Recents keeps whatever was open (MainActivity.onNewIntent tells the two apart).
 * Weak references: a dismissed dialog is forgotten without bookkeeping at each call site.
 */
object OpenDialogs {
    private val open = mutableListOf<WeakReference<Dialog>>()

    fun <T : Dialog> track(dialog: T): T {
        open.removeAll { it.get() == null }
        open += WeakReference(dialog)
        return dialog
    }

    /** Closes the newest first, so a confirmation goes before the page that opened it. */
    fun dismissAll() {
        val dialogs = open.mapNotNull { it.get() }.reversed()
        open.clear()
        dialogs.forEach { if (it.isShowing) runCatching { it.dismiss() } }
    }
}

/** AlertDialog.Builder.show(), tracked by [OpenDialogs]. */
fun AlertDialog.Builder.showTracked(): AlertDialog = OpenDialogs.track(show())
