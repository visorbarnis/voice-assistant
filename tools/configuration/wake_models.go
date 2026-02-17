package main

import (
	"archive/tar"
	"compress/gzip"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

const (
	defaultWakeWordModel = "wn9_mycroft_tts"
	defaultPIOEnv        = "esp32-s3-devkitc-1"
)

type wakeModelCatalog struct {
	FrameworkPath string
	Models        []string
}

type wakeModelProgress func(string)

var wakeModelSanitizer = regexp.MustCompile(`[^A-Z0-9]+`)

func discoverWakeModelCatalog(projectRoot string, progress wakeModelProgress) (wakeModelCatalog, error) {
	catalog := wakeModelCatalog{}
	reportWakeModelProgress(progress, "Load available wake_word models...")
	if projectRoot == "" {
		return catalog, errors.New("project root is empty")
	}

	for _, candidate := range candidateFrameworkPaths(projectRoot) {
		models, err := listWakeModelsInFramework(candidate)
		if err != nil || len(models) == 0 {
			continue
		}
		catalog.FrameworkPath = candidate
		catalog.Models = models
		return catalog, nil
	}

	reportWakeModelProgress(progress, "Local wake-word models not found. Trying to download esp-sr package...")
	frameworkPath, err := ensureCachedFramework(projectRoot, progress)
	if err == nil {
		models, listErr := listWakeModelsInFramework(frameworkPath)
		if listErr == nil && len(models) > 0 {
			catalog.FrameworkPath = frameworkPath
			catalog.Models = models
			reportWakeModelProgress(progress, "Wake-word models are ready.")
			return catalog, nil
		}
	}

	fallback := listWakeModelsFromSdkconfigSymbols(projectRoot)
	if len(fallback) > 0 {
		catalog.Models = fallback
		reportWakeModelProgress(progress, "Wake-word models loaded from sdkconfig symbols.")
		return catalog, nil
	}

	if err != nil {
		return catalog, err
	}
	return catalog, errors.New("wake-word models are unavailable")
}

func detectCurrentWakeModel(projectRoot string) string {
	candidates := []string{
		filepath.Join(projectRoot, "sdkconfig."+defaultPIOEnv),
		filepath.Join(projectRoot, "sdkconfig.defaults"),
		filepath.Join(projectRoot, "sdkconfig"),
	}

	re := regexp.MustCompile(`^CONFIG_WAKE_WORD_MODEL="([^"]+)"$`)
	for _, candidate := range candidates {
		data, err := os.ReadFile(candidate)
		if err != nil {
			continue
		}
		for _, line := range strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n") {
			line = strings.TrimSpace(line)
			m := re.FindStringSubmatch(line)
			if len(m) == 2 {
				model := strings.TrimSpace(m[1])
				if model != "" {
					return model
				}
			}
		}
	}

	return defaultWakeWordModel
}

func applyWakeModelSelection(projectRoot string, catalog wakeModelCatalog, model string) error {
	model = strings.TrimSpace(model)
	if model == "" {
		return errors.New("wake-word model must not be empty")
	}

	if len(catalog.Models) == 0 {
		detected, err := discoverWakeModelCatalog(projectRoot, nil)
		if err != nil {
			return err
		}
		catalog = detected
	}
	if !containsString(catalog.Models, model) {
		return fmt.Errorf("wake-word model '%s' was not found in available models", model)
	}

	frameworkPath := catalog.FrameworkPath
	if frameworkPath == "" {
		var err error
		frameworkPath, err = ensureCachedFramework(projectRoot, nil)
		if err != nil {
			return err
		}
	}

	if err := stageWakeModelForUpload(projectRoot, frameworkPath, model); err != nil {
		return err
	}
	if err := updateWakeModelConfigFiles(projectRoot, model); err != nil {
		return err
	}

	return nil
}

func candidateFrameworkPaths(projectRoot string) []string {
	paths := []string{
		filepath.Join(projectRoot, "managed_components", "espressif__esp-sr"),
		filepath.Join(projectRoot, "components", "esp-sr"),
	}

	cacheRoot := filepath.Join(projectRoot, ".local", "esp-sr-cache")
	entries, err := os.ReadDir(cacheRoot)
	if err == nil {
		cachePaths := make([]string, 0, len(entries))
		for _, entry := range entries {
			if entry.IsDir() && strings.HasPrefix(entry.Name(), "esp-sr-") {
				cachePaths = append(cachePaths, filepath.Join(cacheRoot, entry.Name()))
			}
		}
		sort.Sort(sort.Reverse(sort.StringSlice(cachePaths)))
		paths = append(paths, cachePaths...)
	}

	uniq := make([]string, 0, len(paths))
	seen := make(map[string]struct{}, len(paths))
	for _, path := range paths {
		clean := filepath.Clean(path)
		if _, ok := seen[clean]; ok {
			continue
		}
		seen[clean] = struct{}{}
		uniq = append(uniq, clean)
	}
	return uniq
}

func listWakeModelsInFramework(frameworkPath string) ([]string, error) {
	modelRoot := filepath.Join(frameworkPath, "model", "wakenet_model")
	entries, err := os.ReadDir(modelRoot)
	if err != nil {
		return nil, err
	}

	models := make([]string, 0, len(entries))
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		name := strings.TrimSpace(entry.Name())
		if name == "" || strings.HasPrefix(name, ".") {
			continue
		}
		modelInfo := filepath.Join(modelRoot, name, "_MODEL_INFO_")
		if _, err := os.Stat(modelInfo); err == nil {
			models = append(models, name)
		}
	}
	sort.Strings(models)
	return models, nil
}

func listWakeModelsFromSdkconfigSymbols(projectRoot string) []string {
	files := []string{
		filepath.Join(projectRoot, "sdkconfig."+defaultPIOEnv),
		filepath.Join(projectRoot, "sdkconfig.defaults"),
	}
	modelSet := make(map[string]struct{})

	for _, path := range files {
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		lines := strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n")
		for _, line := range lines {
			symbol, ok := parseWakeWordSymbol(strings.TrimSpace(line))
			if !ok {
				continue
			}
			if strings.HasSuffix(symbol, "_NONE") {
				continue
			}
			model := strings.ToLower(strings.TrimPrefix(symbol, "CONFIG_SR_WN_"))
			model = strings.TrimSuffix(model, "_multi")
			model = strings.TrimSpace(model)
			if model != "" {
				modelSet[model] = struct{}{}
			}
		}
	}

	models := make([]string, 0, len(modelSet))
	for model := range modelSet {
		models = append(models, model)
	}
	sort.Strings(models)
	return models
}

func ensureCachedFramework(projectRoot string, progress wakeModelProgress) (string, error) {
	cacheRoot := filepath.Join(projectRoot, ".local", "esp-sr-cache")
	if err := os.MkdirAll(cacheRoot, 0o755); err != nil {
		return "", err
	}

	version := detectEspSrVersion(projectRoot)
	versionTag := sanitizeVersionTag(version)
	targetPath := filepath.Join(cacheRoot, "esp-sr-"+versionTag)
	if models, err := listWakeModelsInFramework(targetPath); err == nil && len(models) > 0 {
		return targetPath, nil
	}

	urls := []string{
		fmt.Sprintf("https://github.com/espressif/esp-sr/archive/refs/tags/v%s.tar.gz", versionTag),
		fmt.Sprintf("https://github.com/espressif/esp-sr/archive/refs/tags/%s.tar.gz", versionTag),
	}

	var lastErr error
	for _, url := range urls {
		reportWakeModelProgress(progress, fmt.Sprintf("Downloading wake-word package: %s", url))
		archivePath, err := downloadArchiveToTemp(cacheRoot, url)
		if err != nil {
			lastErr = err
			continue
		}

		reportWakeModelProgress(progress, "Extracting wake-word package...")
		extractRoot, err := extractTarGzToTemp(cacheRoot, archivePath)
		_ = os.Remove(archivePath)
		if err != nil {
			lastErr = err
			continue
		}

		frameworkRoot, err := findFrameworkRoot(extractRoot)
		if err != nil {
			_ = os.RemoveAll(extractRoot)
			lastErr = err
			continue
		}

		_ = os.RemoveAll(targetPath)
		if err := os.Rename(frameworkRoot, targetPath); err != nil {
			if copyErr := copyTree(frameworkRoot, targetPath); copyErr != nil {
				_ = os.RemoveAll(extractRoot)
				lastErr = copyErr
				continue
			}
		}
		_ = os.RemoveAll(extractRoot)

		models, listErr := listWakeModelsInFramework(targetPath)
		if listErr == nil && len(models) > 0 {
			reportWakeModelProgress(progress, "Wake-word models are ready.")
			return targetPath, nil
		}
	}

	if lastErr == nil {
		lastErr = errors.New("failed to cache esp-sr framework")
	}
	return "", lastErr
}

func reportWakeModelProgress(progress wakeModelProgress, msg string) {
	if progress == nil {
		return
	}
	progress(strings.TrimSpace(msg))
}

func detectEspSrVersion(projectRoot string) string {
	defaultVersion := "2.2.1"

	lockPath := filepath.Join(projectRoot, "dependencies.lock")
	lockData, err := os.ReadFile(lockPath)
	if err == nil {
		re := regexp.MustCompile(`(?ms)espressif/esp-sr:\s+.*?version:\s*([0-9][0-9A-Za-z.\-_]*)`)
		m := re.FindStringSubmatch(string(lockData))
		if len(m) == 2 {
			return sanitizeVersionTag(m[1])
		}
	}

	manifestPath := filepath.Join(projectRoot, "src", "idf_component.yml")
	manifestData, err := os.ReadFile(manifestPath)
	if err == nil {
		re := regexp.MustCompile(`(?ms)espressif/esp-sr:\s+version:\s*"?([0-9][0-9A-Za-z.\-_]*)"?`)
		m := re.FindStringSubmatch(string(manifestData))
		if len(m) == 2 {
			return sanitizeVersionTag(m[1])
		}
	}

	return defaultVersion
}

func sanitizeVersionTag(v string) string {
	v = strings.TrimSpace(strings.Trim(v, `"'`))
	v = strings.TrimPrefix(v, "v")
	if v == "" {
		return "2.2.1"
	}
	return v
}

func downloadArchiveToTemp(cacheRoot, url string) (string, error) {
	tmpFile, err := os.CreateTemp(cacheRoot, "esp-sr-*.tar.gz")
	if err != nil {
		return "", err
	}
	defer tmpFile.Close()

	client := &http.Client{Timeout: 45 * time.Second}
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		_ = os.Remove(tmpFile.Name())
		return "", err
	}
	req.Header.Set("User-Agent", "settings-configurator/1.0")

	resp, err := client.Do(req)
	if err != nil {
		_ = os.Remove(tmpFile.Name())
		return "", err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		_ = os.Remove(tmpFile.Name())
		return "", fmt.Errorf("download failed (%d): %s", resp.StatusCode, url)
	}

	if _, err := io.Copy(tmpFile, resp.Body); err != nil {
		_ = os.Remove(tmpFile.Name())
		return "", err
	}

	return tmpFile.Name(), nil
}

func extractTarGzToTemp(cacheRoot, archivePath string) (string, error) {
	tmpRoot, err := os.MkdirTemp(cacheRoot, "esp-sr-extract-*")
	if err != nil {
		return "", err
	}

	file, err := os.Open(archivePath)
	if err != nil {
		_ = os.RemoveAll(tmpRoot)
		return "", err
	}
	defer file.Close()

	gz, err := gzip.NewReader(file)
	if err != nil {
		_ = os.RemoveAll(tmpRoot)
		return "", err
	}
	defer gz.Close()

	tr := tar.NewReader(gz)

	for {
		hdr, err := tr.Next()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			_ = os.RemoveAll(tmpRoot)
			return "", err
		}

		name := filepath.Clean(hdr.Name)
		if name == "." || name == "" {
			continue
		}
		if strings.HasPrefix(name, "..") || filepath.IsAbs(name) {
			_ = os.RemoveAll(tmpRoot)
			return "", fmt.Errorf("unsafe archive path: %s", hdr.Name)
		}

		target := filepath.Join(tmpRoot, name)
		rel, err := filepath.Rel(tmpRoot, target)
		if err != nil || strings.HasPrefix(rel, "..") {
			_ = os.RemoveAll(tmpRoot)
			return "", fmt.Errorf("unsafe archive path: %s", hdr.Name)
		}

		switch hdr.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(target, 0o755); err != nil {
				_ = os.RemoveAll(tmpRoot)
				return "", err
			}
		case tar.TypeReg, tar.TypeRegA:
			if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
				_ = os.RemoveAll(tmpRoot)
				return "", err
			}
			out, err := os.OpenFile(target, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
			if err != nil {
				_ = os.RemoveAll(tmpRoot)
				return "", err
			}
			if _, err := io.Copy(out, tr); err != nil {
				_ = out.Close()
				_ = os.RemoveAll(tmpRoot)
				return "", err
			}
			if err := out.Close(); err != nil {
				_ = os.RemoveAll(tmpRoot)
				return "", err
			}
		case tar.TypeSymlink, tar.TypeLink:
			continue
		}
	}

	return tmpRoot, nil
}

func findFrameworkRoot(baseDir string) (string, error) {
	if isFrameworkRoot(baseDir) {
		return baseDir, nil
	}

	var found string
	_ = filepath.WalkDir(baseDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if !d.IsDir() {
			return nil
		}
		if isFrameworkRoot(path) {
			found = path
			return io.EOF
		}
		return nil
	})

	if found == "" {
		return "", errors.New("esp-sr framework root was not found in archive")
	}
	return found, nil
}

func isFrameworkRoot(path string) bool {
	modelScript := filepath.Join(path, "model", "movemodel.py")
	wakenetDir := filepath.Join(path, "model", "wakenet_model")
	if _, err := os.Stat(modelScript); err != nil {
		return false
	}
	info, err := os.Stat(wakenetDir)
	return err == nil && info.IsDir()
}

func stageWakeModelForUpload(projectRoot, frameworkPath, model string) error {
	source := filepath.Join(frameworkPath, "model", "wakenet_model", model)
	if stat, err := os.Stat(source); err != nil || !stat.IsDir() {
		return fmt.Errorf("wake-word model directory not found: %s", source)
	}

	stageTargets := []string{
		filepath.Join(projectRoot, ".local", "wake-models", "upload", "wakenet_model"),
		filepath.Join(projectRoot, ".pio", "build", defaultPIOEnv, "srmodels", "wakenet_model"),
	}

	for _, targetRoot := range stageTargets {
		if err := os.RemoveAll(targetRoot); err != nil {
			return err
		}
		if err := os.MkdirAll(targetRoot, 0o755); err != nil {
			return err
		}
		target := filepath.Join(targetRoot, model)
		if err := copyTree(source, target); err != nil {
			return err
		}
	}

	selectionFile := filepath.Join(projectRoot, ".local", "wake-models", "selected_model.txt")
	if err := os.MkdirAll(filepath.Dir(selectionFile), 0o755); err != nil {
		return err
	}
	return os.WriteFile(selectionFile, []byte(model+"\n"), 0o644)
}

func updateWakeModelConfigFiles(projectRoot, model string) error {
	modelSymbol := modelToWakeWordSymbol(model)
	files := []string{
		filepath.Join(projectRoot, "sdkconfig.defaults"),
		filepath.Join(projectRoot, "sdkconfig."+defaultPIOEnv),
		filepath.Join(projectRoot, "sdkconfig"),
	}

	updatedAny := false
	for _, path := range files {
		if _, err := os.Stat(path); errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err := updateWakeModelInFile(path, model, modelSymbol); err != nil {
			return err
		}
		updatedAny = true
	}

	if !updatedAny {
		return errors.New("sdkconfig files were not found for wake-word update")
	}
	return nil
}

func updateWakeModelInFile(path, model, selectedSymbol string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}

	lines := strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n")
	changed := false
	hasModelLine := false
	hasSelectedSymbol := false

	for i, line := range lines {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" {
			continue
		}

		if strings.HasPrefix(trimmed, "CONFIG_WAKE_WORD_MODEL=") || strings.HasPrefix(trimmed, "# CONFIG_WAKE_WORD_MODEL is not set") {
			next := fmt.Sprintf(`CONFIG_WAKE_WORD_MODEL="%s"`, model)
			if line != next {
				lines[i] = next
				changed = true
			}
			hasModelLine = true
			continue
		}

		symbol, ok := parseWakeWordSymbol(trimmed)
		if !ok {
			continue
		}
		if symbol == selectedSymbol {
			next := symbol + "=y"
			if line != next {
				lines[i] = next
				changed = true
			}
			hasSelectedSymbol = true
			continue
		}
		if strings.HasSuffix(symbol, "_NONE") {
			continue
		}

		next := "# " + symbol + " is not set"
		if line != next {
			lines[i] = next
			changed = true
		}
	}

	if !hasModelLine {
		lines = append(lines, fmt.Sprintf(`CONFIG_WAKE_WORD_MODEL="%s"`, model))
		changed = true
	}
	if !hasSelectedSymbol {
		lines = append(lines, selectedSymbol+"=y")
		changed = true
	}

	if !changed {
		return nil
	}

	output := strings.Join(lines, "\n")
	if !strings.HasSuffix(output, "\n") {
		output += "\n"
	}
	return os.WriteFile(path, []byte(output), 0o644)
}

func parseWakeWordSymbol(line string) (string, bool) {
	if strings.HasPrefix(line, "# CONFIG_SR_WN_") && strings.HasSuffix(line, " is not set") {
		line = strings.TrimPrefix(line, "# ")
		line = strings.TrimSuffix(line, " is not set")
		return strings.TrimSpace(line), true
	}
	if strings.HasPrefix(line, "CONFIG_SR_WN_") {
		if idx := strings.IndexByte(line, '='); idx > 0 {
			return strings.TrimSpace(line[:idx]), true
		}
	}
	return "", false
}

func modelToWakeWordSymbol(model string) string {
	model = strings.ToUpper(strings.TrimSpace(model))
	model = wakeModelSanitizer.ReplaceAllString(model, "_")
	model = strings.Trim(model, "_")
	return "CONFIG_SR_WN_" + model
}

func copyTree(src, dst string) error {
	src = filepath.Clean(src)
	dst = filepath.Clean(dst)

	return filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}

		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)

		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}

		info, err := d.Info()
		if err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}

		in, err := os.Open(path)
		if err != nil {
			return err
		}

		out, err := os.OpenFile(target, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, info.Mode().Perm())
		if err != nil {
			_ = in.Close()
			return err
		}
		if _, err := io.Copy(out, in); err != nil {
			_ = in.Close()
			_ = out.Close()
			return err
		}
		if err := in.Close(); err != nil {
			_ = out.Close()
			return err
		}
		return out.Close()
	})
}

func containsString(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}
