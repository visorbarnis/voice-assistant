package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestModelToWakeWordSymbol(t *testing.T) {
	got := modelToWakeWordSymbol("wn9_mycroft_tts")
	if got != "CONFIG_SR_WN_WN9_MYCROFT_TTS" {
		t.Fatalf("unexpected symbol: %s", got)
	}
}

func TestUpdateWakeModelInFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "sdkconfig.defaults")
	initial := strings.Join([]string{
		`CONFIG_WAKE_WORD_MODEL="wn9_mycroft_tts"`,
		`CONFIG_SR_WN_WN9_MYCROFT_TTS=y`,
		`# CONFIG_SR_WN_WN9_HILEXIN is not set`,
		"",
	}, "\n")

	if err := os.WriteFile(path, []byte(initial), 0o644); err != nil {
		t.Fatalf("write initial file: %v", err)
	}

	if err := updateWakeModelInFile(path, "wn9_hilexin", modelToWakeWordSymbol("wn9_hilexin")); err != nil {
		t.Fatalf("updateWakeModelInFile failed: %v", err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read updated file: %v", err)
	}
	content := string(data)

	if !strings.Contains(content, `CONFIG_WAKE_WORD_MODEL="wn9_hilexin"`) {
		t.Fatalf("wake model line not updated:\n%s", content)
	}
	if !strings.Contains(content, "CONFIG_SR_WN_WN9_HILEXIN=y") {
		t.Fatalf("selected wake symbol not enabled:\n%s", content)
	}
	if !strings.Contains(content, "# CONFIG_SR_WN_WN9_MYCROFT_TTS is not set") {
		t.Fatalf("previous wake symbol not disabled:\n%s", content)
	}
}

func TestApplyWakeModelSelectionStagesAndUpdates(t *testing.T) {
	projectRoot := t.TempDir()

	modelDir := filepath.Join(projectRoot, "managed_components", "espressif__esp-sr", "model", "wakenet_model", "wn9_hilexin")
	if err := os.MkdirAll(modelDir, 0o755); err != nil {
		t.Fatalf("mkdir model dir: %v", err)
	}
	if err := os.WriteFile(filepath.Join(modelDir, "_MODEL_INFO_"), []byte("ok\n"), 0o644); err != nil {
		t.Fatalf("write model info: %v", err)
	}

	sdkconfigPath := filepath.Join(projectRoot, "sdkconfig.defaults")
	sdkconfigData := strings.Join([]string{
		`CONFIG_WAKE_WORD_MODEL="wn9_mycroft_tts"`,
		`CONFIG_SR_WN_WN9_MYCROFT_TTS=y`,
		`# CONFIG_SR_WN_WN9_HILEXIN is not set`,
		"",
	}, "\n")
	if err := os.WriteFile(sdkconfigPath, []byte(sdkconfigData), 0o644); err != nil {
		t.Fatalf("write sdkconfig.defaults: %v", err)
	}

	catalog, err := discoverWakeModelCatalog(projectRoot)
	if err != nil {
		t.Fatalf("discoverWakeModelCatalog: %v", err)
	}

	if err := applyWakeModelSelection(projectRoot, catalog, "wn9_hilexin"); err != nil {
		t.Fatalf("applyWakeModelSelection: %v", err)
	}

	updated, err := os.ReadFile(sdkconfigPath)
	if err != nil {
		t.Fatalf("read updated sdkconfig.defaults: %v", err)
	}
	content := string(updated)
	if !strings.Contains(content, `CONFIG_WAKE_WORD_MODEL="wn9_hilexin"`) {
		t.Fatalf("wake model not updated in sdkconfig.defaults:\n%s", content)
	}

	localCopy := filepath.Join(projectRoot, ".local", "wake-models", "upload", "wakenet_model", "wn9_hilexin", "_MODEL_INFO_")
	if _, err := os.Stat(localCopy); err != nil {
		t.Fatalf("expected staged model copy in .local: %v", err)
	}

	buildCopy := filepath.Join(projectRoot, ".pio", "build", defaultPIOEnv, "srmodels", "wakenet_model", "wn9_hilexin", "_MODEL_INFO_")
	if _, err := os.Stat(buildCopy); err != nil {
		t.Fatalf("expected staged model copy in build path: %v", err)
	}
}
