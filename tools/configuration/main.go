package main

import (
	"encoding/csv"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
)

const (
	appTitle = "ESP32-S3 Settings Configurator (TUI)"
)

type fieldSpec struct {
	Namespace string
	Key       string
	Label     string
	Type      string
	Secret    bool
	Default   string
}

var allFields = []fieldSpec{
	{Namespace: "wifi_settings", Key: "ssid", Label: "WiFi SSID", Type: "string", Default: "your_ssid"},
	{Namespace: "wifi_settings", Key: "password", Label: "WiFi Password", Type: "string", Secret: true, Default: "your_password"},
	{Namespace: "wifi_settings", Key: "max_retry", Label: "WiFi Max Retry", Type: "u32", Default: "5"},

	{Namespace: "server_settings", Key: "host", Label: "Server Host", Type: "string", Default: "192.168.1.100"},
	{Namespace: "server_settings", Key: "location", Label: "Location", Type: "string", Default: ""},
	{Namespace: "server_settings", Key: "parameter", Label: "Parameter", Type: "string", Default: ""},
	{Namespace: "server_settings", Key: "port", Label: "Server Port", Type: "u32", Default: "8100"},
	{Namespace: "server_settings", Key: "ws_path", Label: "WebSocket Path", Type: "string", Default: "/ws/audio"},
	{Namespace: "server_settings", Key: "api_key", Label: "API Key", Type: "string", Secret: true, Default: ""},
	{Namespace: "server_settings", Key: "client_mode", Label: "Client Mode", Type: "string", Default: "half-duplex"},
	{Namespace: "server_settings", Key: "speak_mode", Label: "Speak Mode", Type: "string", Default: "half-duplex"},
	{Namespace: "server_settings", Key: "wake_word_model", Label: "Wake-Word Model", Type: "string", Default: defaultWakeWordModel},
	{Namespace: "server_settings", Key: "wake_mode", Label: "Wake Strictness", Type: "string", Default: "strict"},
	{Namespace: "server_settings", Key: "wake_level", Label: "Wake Sensitivity (0..10)", Type: "u32", Default: "6"},

	{Namespace: "audio_settings", Key: "playback_rate", Label: "Playback Rate (Hz)", Type: "u32", Default: "16000"},
	{Namespace: "audio_settings", Key: "buffer_start_ms", Label: "Buffer Start (ms)", Type: "u32", Default: "100"},
	{Namespace: "audio_settings", Key: "buffer_max_s", Label: "Buffer Max (sec)", Type: "u32", Default: "4"},
}

var namespaceOrder = []string{"wifi_settings", "server_settings", "audio_settings"}

type uiState struct {
	app     *tview.Application
	pages   *tview.Pages
	header  *tview.TextView
	status  *tview.TextView
	summary *tview.TextView
	menu    *tview.List

	path   string
	values map[string]string
	dirty  bool

	projectRoot string
	wakeCatalog wakeModelCatalog
}

func main() {
	fileFlag := flag.String("file", "", "path to settings.csv")
	flag.Parse()

	path := strings.TrimSpace(*fileFlag)
	if path == "" {
		path = detectDefaultSettingsFile()
	}

	values := defaultValues()
	if exists(path) {
		loaded, err := loadSettings(path)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error loading %s: %v\n", path, err)
			os.Exit(1)
		}
		for k, v := range loaded {
			values[k] = v
		}
	}

	projectRoot := filepath.Dir(path)
	wakeCatalog, wakeErr := discoverWakeModelCatalog(projectRoot)
	if wakeErr != nil {
		fmt.Fprintf(os.Stderr, "Wake-word model discovery warning: %v\n", wakeErr)
	}
	values[fieldID("server_settings", "wake_word_model")] = detectCurrentWakeModel(projectRoot)

	state := &uiState{
		app:         tview.NewApplication(),
		pages:       tview.NewPages(),
		header:      tview.NewTextView(),
		status:      tview.NewTextView(),
		path:        path,
		values:      values,
		projectRoot: projectRoot,
		wakeCatalog: wakeCatalog,
	}

	state.buildMainUI()
	state.setStatus("Ready")
	state.refreshAll()

	state.app.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		switch {
		case event.Key() == tcell.KeyCtrlS:
			if err := state.saveCurrent(); err != nil {
				state.showError(err)
			}
			return nil
		case event.Rune() == 'q' || event.Rune() == 'Q':
			state.requestExit()
			return nil
		}
		return event
	})

	if err := state.app.SetRoot(state.pages, true).Run(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
}

func (s *uiState) buildMainUI() {
	s.header.SetDynamicColors(true)
	s.header.SetBorder(true)
	s.header.SetTitle("App")
	s.header.SetTitleAlign(tview.AlignLeft)

	s.menu = tview.NewList()
	s.menu.SetBorder(true)
	s.menu.SetTitle("Menu")
	s.menu.SetTitleAlign(tview.AlignLeft)
	s.menu.ShowSecondaryText(true)

	s.menu.AddItem("Edit WiFi", "ssid / password / retry", '1', func() {
		s.openSpecEditor(
			"WiFi",
			filterFields("wifi_settings"),
			"Tips:\n- WiFi credentials and retry policy\n- values are validated on Apply",
		)
	})
	s.menu.AddItem("Edit Server", "host / location / parameter / api key / wake model / sensitivity", '2', func() {
		s.openSpecEditor(
			"Server",
			pickFields(
				fieldID("server_settings", "host"),
				fieldID("server_settings", "location"),
				fieldID("server_settings", "parameter"),
				fieldID("server_settings", "api_key"),
				fieldID("server_settings", "wake_word_model"),
				fieldID("server_settings", "wake_mode"),
				fieldID("server_settings", "wake_level"),
			),
			"Tips:\n- server identity and authorization\n- wake strictness: normal=DET_MODE_90, strict=DET_MODE_95\n- wake sensitivity: 0 strictest, 10 most sensitive\n- values are validated on Apply",
		)
	})
	s.menu.AddItem("Save", "save to current file", 's', func() {
		if err := s.saveCurrent(); err != nil {
			s.showError(err)
		}
	})
	s.menu.AddItem("Save As", "save to another path", 'a', func() {
		s.openPathPrompt("Save As", s.path, func(path string) {
			if path == "" {
				return
			}
			if !strings.HasSuffix(strings.ToLower(path), ".csv") {
				path += ".csv"
			}
			if err := validateValues(s.values); err != nil {
				s.showError(err)
				return
			}
			if err := writeSettings(path, s.values); err != nil {
				s.showError(err)
				return
			}
			s.path = path
			s.dirty = false
			s.refreshAll()
			s.setStatus("Saved to " + path)
		})
	})
	s.menu.AddItem("Exit", "quit application", 'q', func() {
		s.requestExit()
	})

	s.summary = tview.NewTextView()
	s.summary.SetBorder(true)
	s.summary.SetTitle("Current Values")
	s.summary.SetTitleAlign(tview.AlignLeft)
	s.summary.SetDynamicColors(true)

	s.status.SetBorder(true)
	s.status.SetTitle("Status")
	s.status.SetTitleAlign(tview.AlignLeft)

	content := tview.NewFlex().
		AddItem(s.menu, 34, 1, true).
		AddItem(s.summary, 0, 3, false)

	main := tview.NewFlex().
		SetDirection(tview.FlexRow).
		AddItem(s.header, 3, 0, false).
		AddItem(content, 0, 1, true).
		AddItem(s.status, 3, 0, false)

	s.pages.AddPage("main", main, true, true)
	s.app.SetFocus(s.menu)
}

func (s *uiState) refreshAll() {
	s.refreshHeader()
	s.refreshSummary()
}

func (s *uiState) refreshHeader() {
	dirty := ""
	if s.dirty {
		dirty = " [yellow](unsaved)"
	}
	text := fmt.Sprintf("[::b]%s[::-]\nFile: %s%s\nKeys: Enter select | Ctrl+S save | q exit", appTitle, s.path, dirty)
	s.header.SetText(text)
}

func (s *uiState) refreshSummary() {
	var b strings.Builder
	for _, ns := range namespaceOrder {
		if ns == "audio_settings" {
			continue
		}

		fmt.Fprintf(&b, "[::b]%s[::-]\n", ns)
		for _, spec := range allFields {
			if spec.Namespace != ns {
				continue
			}
			if spec.Namespace == "server_settings" &&
				(spec.Key == "ws_path" || spec.Key == "client_mode" || spec.Key == "speak_mode") {
				continue
			}
			id := fieldID(spec.Namespace, spec.Key)
			v := s.values[id]
			if spec.Secret && v != "" {
				v = maskSecret(v)
			}
			if strings.TrimSpace(v) == "" {
				v = "(empty)"
			}
			fmt.Fprintf(&b, "  %-16s = %s\n", spec.Key, v)
		}
		b.WriteString("\n")
	}
	s.summary.SetText(b.String())
}

func (s *uiState) setStatus(msg string) {
	s.status.SetText(msg)
}

func (s *uiState) loadFromPath(path string) error {
	if !exists(path) {
		return fmt.Errorf("file not found: %s", path)
	}
	loaded, err := loadSettings(path)
	if err != nil {
		return err
	}
	next := defaultValues()
	for k, v := range loaded {
		next[k] = v
	}
	next[fieldID("server_settings", "wake_word_model")] = detectCurrentWakeModel(s.projectRoot)
	if err := validateValues(next); err != nil {
		return err
	}
	s.path = path
	s.values = next
	s.dirty = false
	s.refreshAll()
	return nil
}

func (s *uiState) reloadCurrent() error {
	if !exists(s.path) {
		return fmt.Errorf("file not found: %s", s.path)
	}
	if err := s.loadFromPath(s.path); err != nil {
		return err
	}
	s.setStatus("Reloaded " + s.path)
	return nil
}

func (s *uiState) saveCurrent() error {
	if err := validateValues(s.values); err != nil {
		return err
	}
	if err := writeSettings(s.path, s.values); err != nil {
		return err
	}
	s.dirty = false
	s.refreshAll()
	s.setStatus("Saved " + s.path)
	return nil
}

func (s *uiState) openSpecEditor(title string, specs []fieldSpec, helpText string) {
	if len(specs) == 0 {
		s.showError(fmt.Errorf("no fields configured for %s", title))
		return
	}

	next := cloneValues(s.values)
	getters := make(map[string]func() string)

	form := tview.NewForm()
	form.SetBorder(true)
	form.SetTitle(title + " Editor")
	form.SetTitleAlign(tview.AlignLeft)

	for _, spec := range specs {
		spec := spec
		id := fieldID(spec.Namespace, spec.Key)
		curr := s.values[id]

		if id == fieldID("server_settings", "wake_word_model") {
			models := append([]string{}, s.wakeCatalog.Models...)
			if len(models) == 0 {
				input := tview.NewInputField().
					SetLabel(spec.Label + ": ").
					SetText(curr).
					SetFieldWidth(40)
				form.AddFormItem(input)
				getters[id] = func() string {
					return strings.TrimSpace(input.GetText())
				}
				continue
			}

			selectedModel := strings.TrimSpace(curr)
			if selectedModel == "" {
				selectedModel = detectCurrentWakeModel(s.projectRoot)
			}
			if selectedModel == "" {
				selectedModel = defaultWakeWordModel
			}
			if !containsString(models, selectedModel) {
				models = append([]string{selectedModel}, models...)
			}

			currentIdx := 0
			for i, model := range models {
				if model == selectedModel {
					currentIdx = i
					break
				}
			}

			drop := tview.NewDropDown().
				SetLabel(spec.Label+": ").
				SetOptions(models, nil)
			drop.SetCurrentOption(currentIdx)
			form.AddFormItem(drop)
			getters[id] = func() string {
				idx, _ := drop.GetCurrentOption()
				if idx >= 0 && idx < len(models) {
					return strings.TrimSpace(models[idx])
				}
				return selectedModel
			}
			continue
		}

		if id == fieldID("server_settings", "wake_mode") {
			options := []string{
				"normal (DET_MODE_90)",
				"strict (DET_MODE_95)",
			}
			values := []string{"normal", "strict"}

			mode := strings.ToLower(strings.TrimSpace(curr))
			idx := 1
			if mode == "normal" {
				idx = 0
			}

			drop := tview.NewDropDown().
				SetLabel(spec.Label+": ").
				SetOptions(options, nil)
			drop.SetCurrentOption(idx)
			form.AddFormItem(drop)
			getters[id] = func() string {
				i, _ := drop.GetCurrentOption()
				if i < 0 || i >= len(values) {
					return "strict"
				}
				return values[i]
			}
			continue
		}

		if id == fieldID("server_settings", "wake_level") {
			options := []string{
				"0 (strictest)",
				"1",
				"2",
				"3",
				"4",
				"5",
				"6 (current default)",
				"7",
				"8",
				"9",
				"10 (most sensitive)",
			}

			level, err := strconv.Atoi(strings.TrimSpace(curr))
			if err != nil {
				level = 6
			}
			if level < 0 {
				level = 0
			}
			if level > 10 {
				level = 10
			}

			drop := tview.NewDropDown().
				SetLabel(spec.Label+": ").
				SetOptions(options, nil)
			drop.SetCurrentOption(level)
			form.AddFormItem(drop)
			getters[id] = func() string {
				idx, _ := drop.GetCurrentOption()
				if idx < 0 || idx > 10 {
					return "6"
				}
				return strconv.Itoa(idx)
			}
			continue
		}

		if id == fieldID("server_settings", "client_mode") || id == fieldID("server_settings", "speak_mode") {
			drop := tview.NewDropDown().
				SetLabel(spec.Label+": ").
				SetOptions([]string{"half-duplex", "full-duplex"}, nil)
			if curr == "full-duplex" {
				drop.SetCurrentOption(1)
			} else {
				drop.SetCurrentOption(0)
			}
			form.AddFormItem(drop)
			getters[id] = func() string {
				_, option := drop.GetCurrentOption()
				return strings.TrimSpace(option)
			}
			continue
		}

		input := tview.NewInputField().
			SetLabel(spec.Label + ": ").
			SetText(curr).
			SetFieldWidth(40)

		if spec.Secret {
			input.SetMaskCharacter('*')
		}

		if spec.Type == "u32" {
			input.SetAcceptanceFunc(func(text string, lastChar rune) bool {
				if text == "" {
					return true
				}
				_, err := strconv.ParseUint(text, 10, 32)
				return err == nil
			})
		}

		form.AddFormItem(input)
		getters[id] = func() string {
			return strings.TrimSpace(input.GetText())
		}
	}

	form.AddButton("Apply", func() {
		for id, get := range getters {
			next[id] = get()
		}
		if err := validateValues(next); err != nil {
			s.showError(err)
			return
		}

		currentWakeModel := strings.TrimSpace(s.values[fieldID("server_settings", "wake_word_model")])
		nextWakeModel := strings.TrimSpace(next[fieldID("server_settings", "wake_word_model")])
		if nextWakeModel == "" {
			nextWakeModel = defaultWakeWordModel
			next[fieldID("server_settings", "wake_word_model")] = nextWakeModel
		}
		if currentWakeModel != nextWakeModel {
			s.setStatus("Preparing wake-word model: " + nextWakeModel)
			if err := applyWakeModelSelection(s.projectRoot, s.wakeCatalog, nextWakeModel); err != nil {
				s.showError(err)
				return
			}
			if refreshed, err := discoverWakeModelCatalog(s.projectRoot); err == nil && len(refreshed.Models) > 0 {
				s.wakeCatalog = refreshed
			}
		}

		s.values = next
		s.dirty = true
		s.refreshAll()
		s.pages.SwitchToPage("main")
		s.app.SetFocus(s.menu)
		if currentWakeModel != nextWakeModel {
			s.setStatus(title + " updated; wake-word model applied (unsaved)")
		} else {
			s.setStatus(title + " updated (unsaved)")
		}
	})
	form.AddButton("Cancel", func() {
		s.pages.SwitchToPage("main")
		s.app.SetFocus(s.menu)
	})
	form.SetButtonsAlign(tview.AlignRight)

	help := tview.NewTextView().
		SetDynamicColors(true).
		SetText(helpText)
	help.SetBorder(true)
	help.SetTitle("Help")
	help.SetTitleAlign(tview.AlignLeft)

	layout := tview.NewFlex().
		SetDirection(tview.FlexRow).
		AddItem(form, 0, 4, true).
		AddItem(help, 6, 1, false)

	pageName := "edit-" + strings.ReplaceAll(strings.ToLower(title), " ", "-")
	s.pages.RemovePage(pageName)
	s.pages.AddPage(pageName, center(84, 28, layout), true, true)
	s.pages.SwitchToPage(pageName)
	s.app.SetFocus(form)
}

func (s *uiState) openPathPrompt(title, initial string, onSubmit func(path string)) {
	input := tview.NewInputField().
		SetLabel("Path: ").
		SetText(initial).
		SetFieldWidth(58)

	form := tview.NewForm()
	form.SetBorder(true)
	form.SetTitle(title)
	form.SetTitleAlign(tview.AlignLeft)
	form.AddFormItem(input)
	form.AddButton("OK", func() {
		path := strings.TrimSpace(input.GetText())
		s.pages.RemovePage("path-prompt")
		s.pages.SwitchToPage("main")
		s.app.SetFocus(s.menu)
		onSubmit(path)
	})
	form.AddButton("Cancel", func() {
		s.pages.RemovePage("path-prompt")
		s.pages.SwitchToPage("main")
		s.app.SetFocus(s.menu)
	})
	form.SetButtonsAlign(tview.AlignRight)

	s.pages.RemovePage("path-prompt")
	s.pages.AddPage("path-prompt", center(90, 10, form), true, true)
	s.pages.SwitchToPage("path-prompt")
	s.app.SetFocus(form)
}

func (s *uiState) confirm(text string, onYes func()) {
	modal := tview.NewModal().
		SetText(text).
		AddButtons([]string{"Yes", "No"}).
		SetDoneFunc(func(buttonIndex int, buttonLabel string) {
			s.pages.RemovePage("confirm")
			s.pages.SwitchToPage("main")
			s.app.SetFocus(s.menu)
			if buttonLabel == "Yes" {
				onYes()
			}
		})

	s.pages.RemovePage("confirm")
	s.pages.AddPage("confirm", center(60, 10, modal), true, true)
	s.pages.SwitchToPage("confirm")
	s.app.SetFocus(modal)
}

func (s *uiState) requestExit() {
	if !s.dirty {
		s.app.Stop()
		return
	}

	modal := tview.NewModal().
		SetText("You have unsaved changes. Save before exit?").
		AddButtons([]string{"Save and Exit", "Exit Without Save", "Cancel"}).
		SetDoneFunc(func(buttonIndex int, buttonLabel string) {
			s.pages.RemovePage("exit-confirm")
			s.pages.SwitchToPage("main")
			s.app.SetFocus(s.menu)

			switch buttonLabel {
			case "Save and Exit":
				if err := s.saveCurrent(); err != nil {
					s.showError(err)
					return
				}
				s.app.Stop()
			case "Exit Without Save":
				s.app.Stop()
			}
		})

	s.pages.RemovePage("exit-confirm")
	s.pages.AddPage("exit-confirm", center(70, 11, modal), true, true)
	s.pages.SwitchToPage("exit-confirm")
	s.app.SetFocus(modal)
}

func (s *uiState) showError(err error) {
	msg := err.Error()
	modal := tview.NewModal().
		SetText("Error:\n" + msg).
		AddButtons([]string{"OK"}).
		SetDoneFunc(func(buttonIndex int, buttonLabel string) {
			s.pages.RemovePage("error")
			s.pages.SwitchToPage("main")
			s.app.SetFocus(s.menu)
		})

	s.pages.RemovePage("error")
	s.pages.AddPage("error", center(76, 11, modal), true, true)
	s.pages.SwitchToPage("error")
	s.app.SetFocus(modal)
	s.setStatus("Error: " + msg)
}

func center(width, height int, p tview.Primitive) tview.Primitive {
	return tview.NewFlex().
		AddItem(nil, 0, 1, false).
		AddItem(
			tview.NewFlex().
				SetDirection(tview.FlexRow).
				AddItem(nil, 0, 1, false).
				AddItem(p, height, 1, true).
				AddItem(nil, 0, 1, false),
			width,
			1,
			true,
		).
		AddItem(nil, 0, 1, false)
}

func cloneValues(src map[string]string) map[string]string {
	out := make(map[string]string, len(src))
	for k, v := range src {
		out[k] = v
	}
	return out
}

func filterFields(namespace string) []fieldSpec {
	items := make([]fieldSpec, 0)
	for _, spec := range allFields {
		if spec.Namespace == namespace {
			items = append(items, spec)
		}
	}
	return items
}

func pickFields(ids ...string) []fieldSpec {
	lookup := make(map[string]fieldSpec, len(allFields))
	for _, spec := range allFields {
		lookup[fieldID(spec.Namespace, spec.Key)] = spec
	}

	items := make([]fieldSpec, 0, len(ids))
	for _, id := range ids {
		if spec, ok := lookup[id]; ok {
			items = append(items, spec)
		}
	}
	return items
}

func defaultValues() map[string]string {
	out := make(map[string]string, len(allFields))
	for _, spec := range allFields {
		out[fieldID(spec.Namespace, spec.Key)] = spec.Default
	}
	return out
}

func validateValues(values map[string]string) error {
	for _, spec := range allFields {
		id := fieldID(spec.Namespace, spec.Key)
		val := strings.TrimSpace(values[id])

		switch id {
		case fieldID("wifi_settings", "ssid"):
			if val == "" {
				return errors.New("wifi_settings.ssid must not be empty")
			}
		case fieldID("server_settings", "host"):
			if val == "" {
				return errors.New("server_settings.host must not be empty")
			}
		case fieldID("server_settings", "ws_path"):
			if val == "" {
				return errors.New("server_settings.ws_path must not be empty")
			}
			if !strings.HasPrefix(val, "/") {
				val = "/" + val
				values[id] = val
			}
		case fieldID("server_settings", "client_mode"):
			if val != "half-duplex" && val != "full-duplex" {
				return errors.New("server_settings.client_mode must be 'half-duplex' or 'full-duplex'")
			}
		case fieldID("server_settings", "speak_mode"):
			if val != "half-duplex" && val != "full-duplex" {
				return errors.New("server_settings.speak_mode must be 'half-duplex' or 'full-duplex'")
			}
		case fieldID("server_settings", "wake_mode"):
			if val != "normal" && val != "strict" {
				return errors.New("server_settings.wake_mode must be 'normal' or 'strict'")
			}
		case fieldID("server_settings", "wake_word_model"):
			if val == "" {
				return errors.New("server_settings.wake_word_model must not be empty")
			}
		}

		if spec.Type == "u32" {
			n, err := strconv.ParseUint(val, 10, 32)
			if err != nil {
				return fmt.Errorf("%s must be a positive integer", id)
			}

			switch id {
			case fieldID("server_settings", "port"):
				if n == 0 || n > 65535 {
					return errors.New("server_settings.port must be in range 1..65535")
				}
			case fieldID("server_settings", "wake_level"):
				if n > 10 {
					return errors.New("server_settings.wake_level must be in range 0..10")
				}
			case fieldID("wifi_settings", "max_retry"):
				if n == 0 || n > 100 {
					return errors.New("wifi_settings.max_retry must be in range 1..100")
				}
			case fieldID("audio_settings", "playback_rate"):
				if n < 8000 || n > 96000 {
					return errors.New("audio_settings.playback_rate must be in range 8000..96000")
				}
			case fieldID("audio_settings", "buffer_start_ms"):
				if n > 120000 {
					return errors.New("audio_settings.buffer_start_ms must be in range 0..120000")
				}
			case fieldID("audio_settings", "buffer_max_s"):
				if n == 0 || n > 600 {
					return errors.New("audio_settings.buffer_max_s must be in range 1..600")
				}
			}
		}
	}
	return nil
}

func loadSettings(path string) (map[string]string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	r := csv.NewReader(file)
	r.FieldsPerRecord = -1

	header, err := r.Read()
	if err != nil {
		return nil, err
	}
	if len(header) < 4 {
		return nil, errors.New("invalid CSV header")
	}
	if strings.TrimSpace(header[0]) != "key" || strings.TrimSpace(header[1]) != "type" {
		return nil, errors.New("unsupported CSV format (expected key,type,encoding,value)")
	}

	out := make(map[string]string)
	currentNS := ""

	for {
		rec, err := r.Read()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, err
		}
		if len(rec) < 2 {
			continue
		}

		key := strings.TrimSpace(rec[0])
		typ := strings.TrimSpace(rec[1])
		val := ""
		if len(rec) > 3 {
			val = rec[3]
		}

		switch typ {
		case "namespace":
			currentNS = key
		case "data":
			if currentNS == "" || key == "" {
				continue
			}
			out[fieldID(currentNS, key)] = val
		}
	}

	return out, nil
}

func writeSettings(path string, values map[string]string) error {
	if err := validateValues(values); err != nil {
		return err
	}

	dir := filepath.Dir(path)
	if dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}

	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	w := csv.NewWriter(f)
	defer w.Flush()

	if err := w.Write([]string{"key", "type", "encoding", "value"}); err != nil {
		return err
	}

	for _, ns := range namespaceOrder {
		if err := w.Write([]string{ns, "namespace", "", ""}); err != nil {
			return err
		}
		for _, spec := range allFields {
			if spec.Namespace != ns {
				continue
			}
			id := fieldID(spec.Namespace, spec.Key)
			value := strings.TrimSpace(values[id])
			if err := w.Write([]string{spec.Key, "data", spec.Type, value}); err != nil {
				return err
			}
		}
	}

	return w.Error()
}

func detectDefaultSettingsFile() string {
	cwd, _ := os.Getwd()
	if cwd == "" {
		return "settings.csv"
	}

	current := filepath.Join(cwd, "settings.csv")
	if exists(current) {
		return current
	}

	parent := filepath.Join(filepath.Dir(cwd), "settings.csv")
	if exists(parent) {
		return parent
	}

	return current
}

func exists(path string) bool {
	if path == "" {
		return false
	}
	_, err := os.Stat(path)
	return err == nil
}

func fieldID(ns, key string) string {
	return ns + "." + key
}

func maskSecret(v string) string {
	r := []rune(v)
	if len(r) <= 4 {
		return strings.Repeat("*", len(r))
	}
	return strings.Repeat("*", len(r)-4) + string(r[len(r)-4:])
}
