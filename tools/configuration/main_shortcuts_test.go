package main

import (
	"testing"

	"github.com/gdamore/tcell/v2"
)

func TestShouldHandleExitShortcut(t *testing.T) {
	q := tcell.NewEventKey(tcell.KeyRune, 'q', tcell.ModNone)
	Q := tcell.NewEventKey(tcell.KeyRune, 'Q', tcell.ModNone)
	ctrlQ := tcell.NewEventKey(tcell.KeyRune, 'q', tcell.ModCtrl)
	enter := tcell.NewEventKey(tcell.KeyEnter, 0, tcell.ModNone)

	if !shouldHandleExitShortcut(q, "main", true) {
		t.Fatal("expected q to be handled on main menu")
	}
	if !shouldHandleExitShortcut(Q, "main", true) {
		t.Fatal("expected Q to be handled on main menu")
	}
	if shouldHandleExitShortcut(q, "edit-server", true) {
		t.Fatal("did not expect q to be handled outside main page")
	}
	if shouldHandleExitShortcut(q, "main", false) {
		t.Fatal("did not expect q to be handled when menu is not focused")
	}
	if shouldHandleExitShortcut(ctrlQ, "main", true) {
		t.Fatal("did not expect Ctrl+q to be handled as plain exit shortcut")
	}
	if shouldHandleExitShortcut(enter, "main", true) {
		t.Fatal("did not expect non-rune key to be handled as exit shortcut")
	}
	if shouldHandleExitShortcut(nil, "main", true) {
		t.Fatal("did not expect nil event to be handled")
	}
}
