#include <SDL3/SDL.h>
#include "App.h"
#include "core/SDLPlatform.h" // ProcessSDLEvent (SDL→UIEvent seam, brief 20)
#include <SDL3/SDL_vulkan.h>
#include <cmath>
#include <cstdio>
#include <cctype>

using namespace FluentUI;

App::App(const char *titulo)
    : window(nullptr), ctx(nullptr), m_textInput(""), m_searchText(""), m_multilineText(""),
      m_passwordText(""), m_modalInput(""), m_statusText("Ready") {
    
    std::cout << "Initializing SDL..." << std::endl;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        return;
    }

    // Pick the window flag that matches the configured render backend.
    m_useVulkan = (GetPreferredBackend() == RenderBackendType::Vulkan);

    Uint32 apiFlag = SDL_WINDOW_OPENGL;
    if (m_useVulkan) {
        // If SDL itself has Vulkan support, let it own the surface (needs the
        // SDL_WINDOW_VULKAN flag). If not, create a plain window — the backend
        // builds a native (Win32) surface from the HWND instead.
        if (SDL_Vulkan_LoadLibrary(nullptr)) {
            apiFlag = SDL_WINDOW_VULKAN;
        } else {
            apiFlag = 0;
            std::cout << "SDL has no Vulkan support (" << SDL_GetError()
                      << "); backend will create a native surface." << std::endl;
        }
    }

    m_windowTitle = titulo ? titulo : "";

    std::cout << "Creating Window (" << (m_useVulkan ? "Vulkan" : "OpenGL") << ")..." << std::endl;
    // Borderless so our custom TitleBar() is the window chrome, not the OS one.
    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | apiFlag;
    window = SDL_CreateWindow(titulo, 1280, 720, winFlags);

    if (!window) {
        std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return;
    }

    std::cout << "Creating FluentUI Context..." << std::endl;
    ctx = CreateContext(window);
    if (!ctx) {
        std::cerr << "Failed to create FluentUI Context" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        window = nullptr;
        return;
    }

    // brief 26: plug a host SDL platform so widgets route OS services (cursor,
    // clipboard, IME, OpenURL, window min/max/close) through the platform seam.
    // Host mode = does NOT own SDL_Init/Quit (this example does that itself).
    platform_ = CreateHostPlatform();
    ctx->platform = platform_.get();

    // Custom window chrome: Win11 rounded corners + border, and install the
    // shared title-bar hit test so TitleBar() can drag/resize the window and the
    // caption buttons (min/max/close) work. Requires BuildUI to draw a TitleBar().
    platform_->ApplyBorderlessChrome(window);
    platform_->SetWindowHitTest(window, &CustomTitleBarHitTest, ctx);

    ctx->style = GetDarkFluentStyle();

    // IMPORTANTE: Sincronizar viewport inicial
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    ctx->renderer.SetViewport(w, h);
    
    SDL_StartTextInput(window);
    lastTime = SDL_GetTicks();
    
    std::cout << "Initializing App State..." << std::endl;

    // Initialize tab labels
    m_mainTabLabels = {"Basic", "Input", "Containers", "Lists & Trees",
                       "Overlays", "Theme",
                       "Controls", "Feedback", "Collections", "Layout",
                       "Navigation", "Rich Text", "Cards", "Extras", "Nitidez"};

    m_extraTabLabels = {"Entrada", "Pickers", "Graficas", "Contenedores", "Arbol/Texto"};

    // Editable DataGrid demo data (brief 16): 6 rows x 4 logical columns.
    m_gridRows = {
        {"Alice",   "Engineering", "98",  "true"},
        {"Bob",     "Design",      "87",  "false"},
        {"Carol",   "Marketing",   "75",  "true"},
        {"Dave",    "Sales",       "91",  "false"},
        {"Erin",    "Engineering", "82",  "true"},
        {"Frank",   "Support",     "69",  "false"},
    };

    // NavFrame starts on the "home" page (brief 13).
    NavigateTo(m_navFrame, "home");

    m_containerTabLabels = {"Panel", "ScrollView", "Nested", "Splitter"};

    // ComboBox items
    m_comboItems = {"Option A", "Option B", "Option C", "Option D", "Option E"};
    m_fontItems = {"Segoe UI", "Arial", "Consolas", "Calibri", "Verdana"};

    // ListView items
    m_listItems = {"Dashboard",    "Messages",  "Calendar",
                   "Contacts",     "Settings",  "Notifications",
                   "Tasks",        "Documents", "Photos",
                   "Music",        "Videos",    "Downloads"};

    m_fileListItems = {"main.cpp",      "App.h",       "App.cpp",
                       "Renderer.cpp",  "Context.cpp", "Widgets.h",
                       "FluentTheme.h", "Style.h",     "Layout.h",
                       "CMakeLists.txt"};

    // Accent color names
    m_accentNames = {"Blue", "Green", "Purple", "Orange", "Pink", "Teal"};

    // Rich Text tab: seed the TokenizingTextBox with a few demo chips (brief 17).
    m_tokens = {"C++", "Vulkan", "FluentUI"};
}

App::~App() {
    DestroyContext();
    platform_.reset(); // free the host platform's cursor cache while SDL is still up
    SDL_StopTextInput(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void App::Run() {
    if (!window || !ctx) {
        std::cerr << "Cannot run App: Initialization failed." << std::endl;
        return;
    }

    SDL_Event e;
    bool running = true;
    std::cout << "Starting main loop..." << std::endl;

    while (running) {
        uint64_t currentTime = SDL_GetTicks();
        float deltaTime = (currentTime - lastTime) / 1000.0f;
        deltaTime = std::min(deltaTime, 0.1f);
        lastTime = currentTime;

        ctx->input.Update(window);

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) // TitleBar close button
                running = false;
            else if (e.type == SDL_EVENT_WINDOW_RESIZED) {
                int width, height;
                SDL_GetWindowSize(window, &width, &height);
                ctx->renderer.SetViewport(width, height);
            } else
                ProcessSDLEvent(ctx->input, e);
        }

        // Skip rendering while minimized: the framebuffer is 0x0 and the backend
        // (esp. Vulkan) stalls on a zero-extent swapchain. Idle until restored.
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            continue;
        }

        // Animate progress bar
        if (m_progressAnimating) {
            m_progressValue += deltaTime * 0.15f;
            if (m_progressValue > 1.0f)
                m_progressValue = 0.0f;
        }

        NewFrame(deltaTime);
        BuildUI();
        // brief 15: flush the toast queue once per frame, on the overlay layer,
        // just before rendering.
        RenderToasts(ctx);
        Render();
        // OpenGL presents via the swap; the Vulkan backend presents inside EndFrame().
        if (!m_useVulkan)
            SDL_GL_SwapWindow(window);
    }
}

void App::BuildUI() {
    // Custom window chrome: our TitleBar() drives drag/resize + min/max/close.
    // (The window is borderless and the hit-test was installed in the ctor.)
    ctx->cursorPos = Vec2(0.0f, 0.0f);
    // TitleBar componible (brief 30): el MENÚ va dentro de la barra (izquierda), el
    // buscador al centro, y los caption buttons (min/max/cerrar) a la derecha los
    // añade la propia TitleBar. Sin título. Menús y buscador quedan excluidos del
    // arrastre (los menús a mano, el AutoSuggestBox automáticamente); los huecos que
    // reparten los TitleBarSpacer siguen siendo arrastrables.
    TitleBar("app_titlebar", "", 0, [&] {
        BuildMenuBar();
        TitleBarSpacer();
        AutoSuggestBox("titlebar_search", &m_searchText,
            [](const std::string& q) {
                std::vector<std::string> all = {
                    "Basic", "Input", "Containers", "Lists & Trees", "Overlays",
                    "Theme", "Controls", "Feedback", "Collections", "Layout",
                    "Navigation", "Rich Text", "Cards"};
                if (q.empty()) return all;
                std::string ql;
                for (char ch : q) ql += (char)std::tolower((unsigned char)ch);
                std::vector<std::string> out;
                for (const auto& s : all) {
                    std::string sl;
                    for (char ch : s) sl += (char)std::tolower((unsigned char)ch);
                    if (sl.find(ql) != std::string::npos) out.push_back(s);
                }
                return out;
            }, "Buscar sección...");
        TitleBarSpacer();
    });
    // Alto real de la barra (TitleBar avanza el cursor por su altura). El menú y el
    // buscador ya viven dentro, así que el contenido arranca justo debajo.
    float titleBarH = ctx->lastItemSize.y;

    Vec2 viewport = ctx->renderer.GetViewportSize();

    // ── App CommandBar (barra de herramientas) FUERA de la TitleBar ────────────
    // Franja full-width justo DEBAJO del menú/título (no dentro del chrome). Coloca
    // el cursor en (padding, titleBarH) → GetCurrentAvailableSpace da viewport-2·pad,
    // así el botón de overflow "···" cae a `padding` del borde derecho (simétrico).
    // Se añade para reproducir el layout del editor del game engine y verificar si
    // aquí ocurre el mismo bug.
    float dpi = ctx->dpiScale;
    float cmdBarStripH = 48.0f * dpi;
    ctx->renderer.DrawRectFilled(Vec2(0.0f, titleBarH), Vec2(viewport.x, cmdBarStripH),
                                 ctx->style.panel.headerBackground, 0.0f);
    ctx->cursorPos = Vec2(ctx->style.padding, titleBarH + 4.0f * dpi);
    {
        std::vector<CommandItem> appPrimary = {
            {"New",   Icons::Plus,       [this]{ m_statusText = "CmdBar: New"; },   true, true},
            {"Open",  Icons::FolderOpen, [this]{ m_statusText = "CmdBar: Open"; },  true, true},
            {"Save",  Icons::Save,       [this]{ m_statusText = "CmdBar: Save"; },  true, true},
            {"Cut",   Icons::Scissors,   [this]{ m_statusText = "CmdBar: Cut"; },   true, true},
            {"Copy",  Icons::Copy,       [this]{ m_statusText = "CmdBar: Copy"; },  true, true},
            {"Undo",  Icons::Undo,       [this]{ m_statusText = "CmdBar: Undo"; },  true, true},
            {"Redo",  Icons::Redo,       [this]{ m_statusText = "CmdBar: Redo"; },  true, true},
        };
        std::vector<CommandItem> appSecondary = {
            {"Settings", Icons::Settings,   [this]{ m_statusText = "CmdBar: Settings"; }, false, true},
            {"Help",     Icons::CircleHelp, [this]{ m_statusText = "CmdBar: Help"; },     false, true},
        };
        CommandBar("app_cmdbar", appPrimary, appSecondary);
    }

    // Main content area below the title bar + command bar
    float topChrome = titleBarH + cmdBarStripH;
    Vec2 contentPos(ctx->style.padding, topChrome + ctx->style.padding);
    Vec2 contentSize(viewport.x - ctx->style.padding * 2.0f,
                     viewport.y - topChrome - ctx->style.padding * 2.0f);

    ctx->cursorPos = contentPos;

    // Title
    Label("FluentGUI Widget Gallery", std::nullopt, TypographyStyle::Title);
    Spacing(2);
    Label(m_statusText, std::nullopt, TypographyStyle::Caption, true);
    Spacing(8);

    // Main tab view
    float tabHeight = contentSize.y - 70.0f;
    if (tabHeight < 200.0f)
        tabHeight = 200.0f;

    if (ScopedTabView sc_mainTabs{"main_tabs", &m_mainTab, m_mainTabLabels,
                                  Vec2(contentSize.x, tabHeight)}) {
        switch (m_mainTab) {
        case 0:
            BuildBasicWidgets();
            break;
        case 1:
            BuildInputWidgets();
            break;
        case 2:
            BuildContainers();
            break;
        case 3:
            BuildListsAndTrees();
            break;
        case 4:
            BuildOverlays();
            break;
        case 5:
            BuildThemeSettings();
            break;
        case 6:
            BuildControls();
            break;
        case 7:
            BuildFeedback();
            break;
        case 8:
            BuildCollections();
            break;
        case 9:
            BuildLayout();
            break;
        case 10:
            BuildNavigation();
            break;
        case 11:
            BuildRichText();
            break;
        case 12:
            BuildCards();
            break;
        case 13:
            BuildExtraWidgets();
            break;
        case 14:
            BuildTextSharpnessLab();
            break;
        }
    }

    // Render deferred overlays (dropdowns, context menus)
    RenderDeferredDropdowns();
}

// --- Menu Bar ---------------------------------------------------------------

void App::BuildMenuBar() {
    // Menús inline DENTRO de la TitleBar componible: no usamos BeginMenuBar (que
    // pintaría su propia barra full-width y su propio layout); dibujamos los
    // BeginMenu directamente en el layout horizontal de la titlebar. Como BeginMenu
    // no publica bbox a focusableWidgets, su zona no se auto-excluye del arrastre,
    // así que la marcamos a mano con TitleBarDragExclude (además de evitar el drag,
    // esto permite que el clic llegue al menú en vez de tragárselo como HTCAPTION).
    float menuX0 = ctx->cursorPos.x;

    if (ScopedMenu sc_menuFile{"File"}) {
        if (MenuItem("New"))  m_statusText = "File > New clicked";
        if (MenuItem("Open")) m_statusText = "File > Open clicked";
        if (MenuItem("Save")) m_statusText = "File > Save clicked";
        MenuSeparator();
        if (MenuItem("Exit")) m_statusText = "File > Exit clicked";
    }
    if (ScopedMenu sc_menuEdit{"Edit"}) {
        if (MenuItem("Undo")) m_statusText = "Edit > Undo clicked";
        if (MenuItem("Redo")) m_statusText = "Edit > Redo clicked";
        MenuSeparator();
        if (MenuItem("Cut"))   m_statusText = "Edit > Cut clicked";
        if (MenuItem("Copy"))  m_statusText = "Edit > Copy clicked";
        if (MenuItem("Paste")) m_statusText = "Edit > Paste clicked";
    }
    if (ScopedMenu sc_menuView{"View"}) {
        if (MenuItem("Toggle Theme")) {
            m_isDarkTheme = !m_isDarkTheme;
            ctx->style =
                m_isDarkTheme ? GetDarkFluentStyle() : GetDefaultFluentStyle();
            m_statusText =
                m_isDarkTheme ? "Switched to Dark Theme" : "Switched to Light Theme";
        }
        MenuSeparator();
        if (MenuItem("Zoom In"))  m_statusText = "View > Zoom In";
        if (MenuItem("Zoom Out")) m_statusText = "View > Zoom Out";
    }
    if (ScopedMenu sc_menuHelp{"Help"}) {
        if (MenuItem("About")) m_modalOpen = true;
        if (MenuItem("Documentation")) m_statusText = "Help > Documentation";
    }

    // Excluir del arrastre toda la franja ocupada por los menús. La altura (200) se
    // recorta sola contra el rect del caption, así que sobra-cubrir es inofensivo.
    float menuX1 = ctx->cursorPos.x;
    TitleBarDragExclude(Rect(Vec2(menuX0, 0.0f), Vec2(menuX1 - menuX0, 200.0f)));
}

// --- Tab 0: Basic Widgets ---------------------------------------------------

void App::BuildBasicWidgets() {
    // --- Buttons section ---
    Label("Buttons", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);

    Horizontal(8.0f, [&] {
        if (Button("Primary Button")) {
            m_buttonClickCount++;
            m_statusText =
                "Button clicked " + std::to_string(m_buttonClickCount) + " times";
        }
        if (Button("Secondary")) {
            m_statusText = "Secondary button clicked";
        }
        Button("Disabled", Vec2(0, 0), std::nullopt, false);
    });

    Spacing(4);
    char clickBuf[64];
    snprintf(clickBuf, sizeof(clickBuf), "Click count: %d", m_buttonClickCount);
    Label(clickBuf, std::nullopt, TypographyStyle::Caption);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Labels section ---
    Label("Typography Variants", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Label("Title Large", std::nullopt, TypographyStyle::TitleLarge);
    Label("Title", std::nullopt, TypographyStyle::Title);
    Label("Subtitle", std::nullopt, TypographyStyle::Subtitle);
    Label("Body (default)", std::nullopt, TypographyStyle::Body);
    Label("Body Strong", std::nullopt, TypographyStyle::BodyStrong);
    Label("Caption text", std::nullopt, TypographyStyle::Caption);
    Label("Disabled label", std::nullopt, TypographyStyle::Body, true);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Checkboxes ---
    Label("Checkboxes", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (Checkbox("Enable notifications", &m_checkbox1)) {
        m_statusText = m_checkbox1 ? "Notifications enabled" : "Notifications disabled";
    }
    if (Checkbox("Dark mode (synced)", &m_isDarkTheme)) {
        // Preservar el acento elegido al cambiar de tema (no resetear a azul).
        Color accents[] = {FluentColors::AccentBlue,  FluentColors::AccentGreen,
                           FluentColors::AccentPurple, FluentColors::AccentOrange,
                           FluentColors::AccentPink,   FluentColors::AccentTeal};
        ctx->style = CreateCustomFluentStyle(accents[m_accentColorIdx], m_isDarkTheme);
        m_statusText =
            m_isDarkTheme ? "Dark mode ON" : "Dark mode OFF";
    }
    Checkbox("Remember me", &m_checkbox3);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Radio Buttons ---
    Label("Radio Buttons", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    RadioButton("Small", &m_radioSelection, 0, "size");
    RadioButton("Medium", &m_radioSelection, 1, "size");
    RadioButton("Large", &m_radioSelection, 2, "size");
    RadioButton("Extra Large", &m_radioSelection, 3, "size");

    Spacing(4);
    const char *sizeNames[] = {"Small", "Medium", "Large", "Extra Large"};
    std::string radioStatus = std::string("Selected size: ") + sizeNames[m_radioSelection];
    Label(radioStatus, std::nullopt, TypographyStyle::Caption);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Progress Bars ---
    Label("Progress Bars", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);

    char progBuf[32];
    snprintf(progBuf, sizeof(progBuf), "%.0f%%", m_progressValue * 100.0f);
    ProgressBar(m_progressValue, Vec2(400, 20), progBuf);
    Spacing(2);
    ProgressBar(0.75f, Vec2(400, 8));
    Spacing(2);
    ProgressBar(0.33f, Vec2(400, 14), "Loading...");

    Spacing(4);
    if (Checkbox("Animate progress", &m_progressAnimating)) {
        m_statusText =
            m_progressAnimating ? "Progress animating" : "Progress paused";
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Separators ---
    Label("Separators", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Label("Content above separator");
    Separator();
    Label("Content below separator");
}

// --- Tab 1: Input Widgets ---------------------------------------------------

void App::BuildInputWidgets() {
    // --- Sliders ---
    Label("Sliders", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    SliderFloat("Opacity", &m_sliderFloat, 0.0f, 1.0f, 300.0f, "%.2f");
    SliderInt("Quantity", &m_sliderInt, 0, 100, 300.0f);
    SliderFloat("Volume", &m_sliderVolume, 0.0f, 100.0f, 300.0f, "%.0f");

    Spacing(4);
    char sliderBuf[128];
    snprintf(sliderBuf, sizeof(sliderBuf),
             "Opacity: %.2f  |  Quantity: %d  |  Volume: %.0f",
             m_sliderFloat, m_sliderInt, m_sliderVolume);
    Label(sliderBuf, std::nullopt, TypographyStyle::Caption);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Text Input ---
    Label("Text Input", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    TextInput("Username", &m_textInput, 300.0f);
    Spacing(2);
    TextInput("Search", &m_searchText, 300.0f);
    Spacing(2);
    TextInput("Password", &m_passwordText, 300.0f);

    Spacing(4);
    if (!m_textInput.empty()) {
        std::string inputStatus = "Input: " + m_textInput;
        Label(inputStatus, std::nullopt, TypographyStyle::Caption);
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Multiline Text ---
    Label("Multiline Text Input", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    TextInput("Notes", &m_multilineText, 400.0f, true);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- ComboBox ---
    Label("ComboBox / Dropdown", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    ComboBox("Select option", &m_comboSelection, m_comboItems, 250.0f);
    Spacing(2);
    ComboBox("Font family", &m_comboFont, m_fontItems, 250.0f);

    Spacing(4);
    std::string comboStatus =
        "Selected: " + m_comboItems[m_comboSelection] +
        "  |  Font: " + m_fontItems[m_comboFont];
    Label(comboStatus, std::nullopt, TypographyStyle::Caption);
}

// --- Tab 2: Containers ------------------------------------------------------

void App::BuildContainers() {
    if (ScopedTabView sc_ctnTabs{"container_tabs", &m_containerTab, m_containerTabLabels,
                                 Vec2(0, 400)}) {
        switch (m_containerTab) {
        case 0: {
            // --- Panels ---
            Label("Panels", std::nullopt, TypographyStyle::Subtitle);
            Spacing(4);

            if (ScopedPanel sc_p1{"demo_panel_1", Vec2(400, 120)}) {
                Label("Standard Panel", std::nullopt, TypographyStyle::BodyStrong);
                Spacing(2);
                Label("This is a standard panel with default styling.");
                Spacing(2);
                if (Button("Panel Button")) {
                    m_statusText = "Panel button clicked";
                }
            }

            Spacing(8);

            if (ScopedPanel sc_pAcr{"demo_panel_acrylic", Vec2(400, 100), true, true, 0.7f}) {
                Label("Acrylic Panel", std::nullopt, TypographyStyle::BodyStrong);
                Spacing(2);
                Label("Panel with acrylic blur effect enabled.");
            }

            Spacing(8);

            if (ScopedPanel sc_p3{"demo_panel_3", Vec2(400, 80)}) {
                Label("Compact Panel", std::nullopt, TypographyStyle::BodyStrong);
                Label("Panels auto-size to content.");
            }
            break;
        }
        case 1: {
            // --- ScrollView ---
            Label("ScrollView", std::nullopt, TypographyStyle::Subtitle);
            Spacing(4);
            Label("Scroll down to see more content:", std::nullopt,
                  TypographyStyle::Caption);
            Spacing(4);

            if (ScopedScrollView sc_scroll{"demo_scroll", Vec2(450, 250), &m_scrollOffset}) {
                for (int i = 0; i < 30; i++) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Scrollable item #%d", i + 1);
                    Label(buf);
                    if (i < 29)
                        Spacing(2);
                }
            }

            Spacing(4);
            char scrollBuf[64];
            snprintf(scrollBuf, sizeof(scrollBuf), "Scroll Y: %.1f",
                     m_scrollOffset.y);
            Label(scrollBuf, std::nullopt, TypographyStyle::Caption);
            break;
        }
        case 2: {
            // --- Nested containers ---
            Label("Nested Containers", std::nullopt, TypographyStyle::Subtitle);
            Spacing(4);

            if (ScopedPanel sc_outer{"outer_panel", Vec2(500, 280)}) {
                Label("Outer Panel", std::nullopt, TypographyStyle::BodyStrong);
                Spacing(4);

                Horizontal(8.0f, [&] {
                    if (ScopedPanel sc_inner1{"inner_panel_1", Vec2(220, 180)}) {
                        Label("Inner Panel 1", std::nullopt, TypographyStyle::Caption);
                        Spacing(2);
                        Checkbox("Option A", &m_checkbox1);
                        Checkbox("Option B", &m_checkbox3);
                        Spacing(2);
                        if (Button("Action", Vec2(100, 0))) {
                            m_statusText = "Inner panel 1 action";
                        }
                    }
                    if (ScopedPanel sc_inner2{"inner_panel_2", Vec2(220, 180)}) {
                        Label("Inner Panel 2", std::nullopt, TypographyStyle::Caption);
                        Spacing(2);
                        SliderFloat("Value", &m_sliderFloat, 0.0f, 1.0f, 180.0f);
                        Spacing(2);
                        ProgressBar(m_sliderFloat, Vec2(180, 12));
                    }
                });
            }
            break;
        }
        case 3: {
            // --- Splitter (replica un layout de editor: splitters anidados) ------
            // Reproduce el patrón del game engine: split_main (izq | resto), dentro
            // split_right (centro | inspector), dentro split_bottom (viewport / consola).
            // Cada pane usa CONTENIDO DIRECTO (Label + widgets), NO BeginPanel.
            Label("Splitter (layout tipo editor)", std::nullopt, TypographyStyle::Subtitle);
            Spacing(4);
            Label("Splitters anidados con contenido directo en cada pane. Arrastra los "
                  "divisores para redimensionar.", std::nullopt, TypographyStyle::Caption);
            Spacing(2);
            Label("CLAVE: el Splitter NO pinta el fondo de sus regiones. Cada pane "
                  "acoplado pinta el suyo (DrawRectFilled). El 'Viewport' se deja SIN "
                  "fondo a proposito -> muestra el fondo de la ventana, como en un editor.",
                  std::nullopt, TypographyStyle::Caption);
            Spacing(8);

            static float s_splitMain = 0.24f;   // jerarquia (izq) | resto
            static float s_splitRight = 0.72f;  // centro | inspector (der)
            static float s_splitBottom = 0.62f; // viewport (arriba) | consola (abajo)

            UIContext* c = GetContext();
            // Tamaño de la region actual del splitter (= availableSpace del pane), medido
            // al ENTRAR al pane (antes de dibujar nada). Equivale al paneSize() del engine.
            auto paneSize = [](UIContext* cc) -> Vec2 {
                return (cc && !cc->layoutStack.empty())
                           ? cc->layoutStack.back().availableSpace
                           : Vec2(0.0f, 0.0f);
            };
            // Pane acoplado: pinta su fondo (lo que faltaba en el engine) + titulo.
            auto dockPane = [&](const char* title) {
                Vec2 origin = c->cursorPos;
                Vec2 ps = paneSize(c);
                c->renderer.DrawRectFilled(origin, ps, c->style.panel.background, 0.0f);
                Label(title, std::nullopt, TypographyStyle::BodyStrong);
                Separator();
            };

            if (c && BeginSplitter("demo_split_main", true, &s_splitMain, Vec2(600, 320))) {
                // Izquierda: Jerarquia
                dockPane("Jerarquia");
                Label("Entidad 1", Icons::Box, std::nullopt, TypographyStyle::Caption);
                Label("Entidad 2", Icons::Box, std::nullopt, TypographyStyle::Caption);
                Label("Entidad 3", Icons::Box, std::nullopt, TypographyStyle::Caption);
                SplitterPanel();

                // Derecha: centro | inspector
                if (BeginSplitter("demo_split_right", true, &s_splitRight)) {
                    // Centro: viewport (arriba) | consola (abajo)
                    if (BeginSplitter("demo_split_bottom", false, &s_splitBottom)) {
                        // Viewport: SIN fondo, a proposito -> se ve el fondo de la ventana.
                        Label("Viewport (sin fondo, transparente)", std::nullopt,
                              TypographyStyle::Caption);
                        SplitterPanel();
                        // Consola
                        dockPane("Consola");
                        Label("[info] Escena cargada", std::nullopt, TypographyStyle::Caption);
                        Label("[warn] Textura no encontrada", std::nullopt,
                              TypographyStyle::Caption);
                        EndSplitter();
                    }
                    SplitterPanel();
                    // Inspector
                    dockPane("Inspector");
                    Label("Transform", std::nullopt, TypographyStyle::Caption);
                    SliderFloat("PosX", &m_sliderFloat, 0.0f, 1.0f, 160.0f);
                    EndSplitter();
                }
                EndSplitter();
            }
            break;
        }
        }
    }
}

// --- Tab 3: Lists & Trees ---------------------------------------------------

void App::BuildListsAndTrees() {
    Label("ListView & TreeView", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);

    Horizontal(12.0f, [&] {

    // ListView 1 - Navigation
    Vertical(4.0f, [&] {
    Label("Navigation", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(2);
    { ScopedListView sc_navList{"nav_list", Vec2(200, 300), &m_listSelection, m_listItems}; }
    Spacing(2);
    std::string listStatus = "Selected: " + m_listItems[m_listSelection];
    Label(listStatus, std::nullopt, TypographyStyle::Caption);
    });

    // ListView 2 - Files
    Vertical(4.0f, [&] {
    Label("Files", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(2);
    { ScopedListView sc_fileList{"file_list", Vec2(200, 300), &m_fileListSelection,
                  m_fileListItems}; }
    Spacing(2);
    if (m_fileListSelection >= 0 &&
        m_fileListSelection < (int)m_fileListItems.size()) {
        std::string fileStatus = "File: " + m_fileListItems[m_fileListSelection];
        Label(fileStatus, std::nullopt, TypographyStyle::Caption);
    }
    });

    // TreeView
    Vertical(4.0f, [&] {
    Label("Project Explorer", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(2);
    if (ScopedTreeView sc_tree{"project_tree", Vec2(250, 300)}) {
        if (TreeNode("src", "src/", &m_treeRoot1Open)) {
            TreeNodePush();
            if (TreeNode("core", "Core/", &m_treeSub1Open)) {
                TreeNodePush();
                TreeNode("renderer", "Renderer.cpp", nullptr, &m_treeSelected1);
                TreeNode("context", "Context.cpp", nullptr, &m_treeSelected2);
                TreeNodePop();
            }
            if (TreeNode("ui", "UI/", &m_treeSub2Open)) {
                TreeNodePush();
                TreeNode("widgets", "Widgets.cpp", nullptr, &m_treeSelected3);
                TreeNode("layout", "Layout.cpp", nullptr, &m_treeSelected4);
                TreeNodePop();
            }
            TreeNodePop();
        }
        if (TreeNode("include", "include/", &m_treeRoot2Open)) {
            TreeNodePush();
            TreeNode("fluentgui_h", "FluentGUI.h", nullptr, &m_treeSelected5);
            TreeNodePop();
        }
        if (TreeNode("examples", "examples/", &m_treeRoot3Open)) {
            TreeNodePush();
            TreeNode("app_h", "App.h");
            TreeNode("app_cpp", "App.cpp");
            TreeNodePop();
        }
    }
    });

    });
}

// --- Tab 4: Overlays --------------------------------------------------------

void App::BuildOverlays() {
    // --- Modals ---
    Label("Modal Dialogs", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);

    Horizontal(8.0f, [&] {
        if (Button("Open About Modal")) {
            m_modalOpen = true;
        }
        if (Button("Open Confirm Modal")) {
            m_confirmModalOpen = true;
        }
    });

    // About modal
    if (ScopedModal sc_modalAbout{"about_modal", "About FluentGUI", &m_modalOpen,
                   Vec2(420, 250)}) {
        Label("FluentGUI v1.0", std::nullopt, TypographyStyle::Title);
        Spacing(4);
        Label("An ImGui-style UI library with Fluent Design styling.");
        Spacing(2);
        Label("Built with C++20, OpenGL 4.5, SDL3, FreeType.",
              std::nullopt, TypographyStyle::Caption);
        Spacing(8);
        TextInput("Feedback", &m_modalInput, 350.0f);
        Spacing(8);
        if (Button("Close", Vec2(100, 0))) {
            m_modalOpen = false;
            if (!m_modalInput.empty()) {
                m_statusText = "Feedback: " + m_modalInput;
            }
        }
    }

    // Confirm modal
    if (ScopedModal sc_modalConfirm{"confirm_modal", "Confirm Action", &m_confirmModalOpen,
                   Vec2(350, 180)}) {
        Label("Are you sure you want to proceed?");
        Label("This action cannot be undone.", std::nullopt,
              TypographyStyle::Caption, true);
        Spacing(12);
        Horizontal(8.0f, [&] {
            if (Button("Yes, Proceed")) {
                m_confirmModalOpen = false;
                m_statusText = "Action confirmed";
            }
            if (Button("Cancel")) {
                m_confirmModalOpen = false;
                m_statusText = "Action cancelled";
            }
        });
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Tooltips ---
    Label("Tooltips", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Label("Hover over the buttons below to see tooltips:");
    Spacing(4);

    Horizontal(8.0f, [&] {
        Button("Save File");
        Tooltip("Save the current file to disk (Ctrl+S)", 0.3f);

        Button("Delete");
        Tooltip("Permanently delete the selected item", 0.3f);

        Button("Settings");
        Tooltip("Open application settings", 0.3f);

        Button("Info");
        Tooltip("Shows detailed information about the selected item.", 0.5f);
    });

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Context Menu ---
    Label("Context Menu", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Label("Right-click anywhere to open the context menu.");
    Spacing(4);

    if (ScopedPanel sc_ctxArea{"ctx_menu_area", Vec2(400, 120)}) {
        Label("Right-click inside this panel", std::nullopt,
              TypographyStyle::Caption, true);
        Spacing(4);
        Label("Context menus appear at the mouse position.");

        if (ScopedContextMenu sc_ctx{"demo_context"}) {
            if (ContextMenuItem("Cut"))
                m_statusText = "Context: Cut";
            if (ContextMenuItem("Copy"))
                m_statusText = "Context: Copy";
            if (ContextMenuItem("Paste"))
                m_statusText = "Context: Paste";
            ContextMenuSeparator();
            if (ContextMenuItem("Select All"))
                m_statusText = "Context: Select All";
            ContextMenuItem("Disabled Item", false);
        }
    }
}

// --- Tab 5: Theme -----------------------------------------------------------

void App::BuildThemeSettings() {
    Label("Theme Settings", std::nullopt, TypographyStyle::Subtitle);
    Spacing(8);

    // Theme toggle
    Label("Base Theme", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(4);
    Horizontal(8.0f, [&] {
        if (Button(m_isDarkTheme ? "Switch to Light" : "Switch to Dark")) {
            m_isDarkTheme = !m_isDarkTheme;
            // Reapply with current accent
            Color accents[] = {FluentColors::AccentBlue,  FluentColors::AccentGreen,
                               FluentColors::AccentPurple, FluentColors::AccentOrange,
                               FluentColors::AccentPink,   FluentColors::AccentTeal};
            ctx->style = CreateCustomFluentStyle(accents[m_accentColorIdx], m_isDarkTheme);
            m_statusText =
                m_isDarkTheme ? "Dark theme applied" : "Light theme applied";
        }
        Label(m_isDarkTheme ? "Current: Dark" : "Current: Light", std::nullopt,
              TypographyStyle::Caption);
    });

    Spacing(12);
    Separator();
    Spacing(8);

    // Accent color selection
    Label("Accent Color", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(4);

    Color accents[] = {FluentColors::AccentBlue,  FluentColors::AccentGreen,
                       FluentColors::AccentPurple, FluentColors::AccentOrange,
                       FluentColors::AccentPink,   FluentColors::AccentTeal};

    for (int i = 0; i < 6; i++) {
        if (RadioButton(m_accentNames[i], &m_accentColorIdx, i, "accent")) {
            // Aplicar en el acto al elegir el color (no esperar a "Apply").
            ctx->style = CreateCustomFluentStyle(accents[m_accentColorIdx], m_isDarkTheme);
            m_statusText = "Accent: " + m_accentNames[i];
        }
    }

    Spacing(4);
    if (Button("Apply Accent Color")) {
        ctx->style = CreateCustomFluentStyle(accents[m_accentColorIdx], m_isDarkTheme);
        m_statusText = "Accent: " + m_accentNames[m_accentColorIdx];
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // Preview widgets with current theme
    Label("Theme Preview", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(4);

    if (ScopedPanel sc_preview{"theme_preview", Vec2(500, 200)}) {
        Label("Preview Panel", std::nullopt, TypographyStyle::Subtitle);
        Spacing(4);

        Horizontal(8.0f, [&] {
            Button("Normal");
            Button("Hover me");
            Button("Disabled", Vec2(0, 0), std::nullopt, false);
        });

        Spacing(4);
        ProgressBar(0.6f, Vec2(400, 16), "60%");
        Spacing(4);

        static bool previewCheck = true;
        static float previewSlider = 0.5f;
        Checkbox("Preview checkbox", &previewCheck);
        SliderFloat("Preview slider", &previewSlider, 0.0f, 1.0f, 300.0f);
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // Layout demo
    Label("Layout Demo", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(4);

    Horizontal(4.0f, [&] {
        Button("A");
        Button("B");
        Button("C");
        Button("D");
        Button("E");
    });

    Spacing(4);

    Horizontal(16.0f, [&] {
        Button("Wide spacing");
        Button("Between");
        Button("Buttons");
    });
}

// --- Tab 6: Signature Controls (brief 14) -----------------------------------

void App::BuildControls() {
    // --- ToggleSwitch ---
    Label("ToggleSwitch", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (ToggleSwitch("Wi-Fi", &m_toggleWifi, "On", "Off")) {
        m_statusText = m_toggleWifi ? "Wi-Fi enabled" : "Wi-Fi disabled";
    }
    if (ToggleSwitch("Bluetooth", &m_toggleBluetooth, "On", "Off")) {
        m_statusText = m_toggleBluetooth ? "Bluetooth enabled" : "Bluetooth disabled";
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Expander ---
    Label("Expander", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (ScopedExpander sc_exp{"demo_expander", "Advanced settings", Icons::Settings,
                      &m_expanderOpen}) {
        Label("These options are hidden until the card is expanded.");
        Spacing(2);
        Checkbox("Enable telemetry", &m_checkbox1);
        SliderInt("Cache size (MB)", &m_sliderInt, 0, 100, 240.0f);
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- SplitButton / DropDownButton ---
    Label("SplitButton & DropDownButton", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);

    std::vector<CommandItem> saveMenu = {
        {"Save", Icons::Save, [this]() { m_statusText = "Save"; }, true, true},
        {"Save As...", Icons::Copy, [this]() { m_statusText = "Save As"; }, false, true},
        {"Save All", Icons::FileText, [this]() { m_statusText = "Save All"; }, false, true},
    };
    Horizontal(12.0f, [&] {
        SplitButton("Save", Icons::Save, [this]() { m_statusText = "Primary Save"; }, saveMenu);

        std::vector<CommandItem> exportMenu = {
            {"Export PNG", Icons::Image, [this]() { m_statusText = "Export PNG"; }, true, true},
            {"Export PDF", Icons::FileText, [this]() { m_statusText = "Export PDF"; }, true, true},
            {"Export SVG", Icons::FileText, [this]() { m_statusText = "Export SVG"; }, true, true},
        };
        DropDownButton("Export", Icons::Download, exportMenu);
    });

    Spacing(12);
    Separator();
    Spacing(8);

    // --- NumberBox ---
    Label("NumberBox", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (NumberBox("Temperature (C)", &m_numberBoxValue, -50.0, 150.0, 1.0, "%.0f")) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Temperature set to %.0f", m_numberBoxValue);
        m_statusText = buf;
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- RatingControl ---
    Label("RatingControl", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (RatingControl("demo_rating", &m_rating, 5, false)) {
        m_statusText = "Rating: " + std::to_string(m_rating) + " of 5";
    }
    Spacing(2);
    Label("Half-stars:", std::nullopt, TypographyStyle::Caption);
    if (RatingControl("demo_rating_half", &m_ratingHalf, 5, true)) {
        m_statusText = "Half rating units: " + std::to_string(m_ratingHalf);
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Flyout / MenuFlyout ---
    Label("Flyout & MenuFlyout", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    // brief 31: se conserva la primitiva Begin/End aquí a propósito — flyoutAnchor y
    // menuAnchor se declaran dentro de la fila pero se usan DESPUÉS (al abrir los
    // flyouts), así que envolver el cuerpo en un lambda/scope los ocultaría. Es
    // justo el caso para el que la primitiva sigue existiendo.
    BeginHorizontal(12.0f);
    if (Button("Open Flyout", Icons::Info)) {
        OpenFlyout("demo_flyout");
    }
    Rect flyoutAnchor(ctx->lastItemPos, ctx->lastItemSize);

    if (Button("Menu Flyout", Icons::LayoutGrid)) {
        OpenFlyout("demo_menuflyout");
    }
    Rect menuAnchor(ctx->lastItemPos, ctx->lastItemSize);
    EndHorizontal();

    // Generic anchored flyout with arbitrary content.
    if (ScopedFlyout sc_flyout{"demo_flyout", flyoutAnchor, FlyoutPlacement::Bottom}) {
        Label("Quick actions", std::nullopt, TypographyStyle::BodyStrong);
        Spacing(4);
        if (Button("Refresh", Vec2(160, 0))) {
            m_statusText = "Flyout: Refresh";
            CloseFlyout("demo_flyout");
        }
        if (Button("Reset", Vec2(160, 0))) {
            m_statusText = "Flyout: Reset";
            CloseFlyout("demo_flyout");
        }
    }

    // Menu-style flyout (icon/label/accelerator rows).
    std::vector<MenuEntry> menuEntries = {
        {"Cut", Icons::Copy, "Ctrl+X", false, false, false, true, {}, [this]() { m_statusText = "Menu: Cut"; }},
        {"Copy", Icons::Copy, "Ctrl+C", false, false, false, true, {}, [this]() { m_statusText = "Menu: Copy"; }},
        {"Paste", Icons::Copy, "Ctrl+V", false, false, false, true, {}, [this]() { m_statusText = "Menu: Paste"; }},
        {"", 0, "", false, false, true, true, {}, nullptr}, // separator
        {"Word wrap", 0, "", true, m_checkbox1, false, true, {}, [this]() { m_checkbox1 = !m_checkbox1; }},
    };
    MenuFlyout("demo_menuflyout", menuAnchor, menuEntries);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- ContentDialog ---
    Label("ContentDialog", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (Button("Open Content Dialog")) {
        m_contentDialogOpen = true;
    }
    // ContentDialog se llama SIEMPRE (no envuelto en `if (open)`): él mismo se
    // dibuja solo cuando está abierto, y necesita los frames posteriores al cierre
    // para animar la salida y liberar la captura de input del modal.
    {
        DialogResult r = ContentDialog(
            "demo_content_dialog", &m_contentDialogOpen, "Rename item",
            [this]() {
                Label("Enter a new name for the selected item:");
                Spacing(6);
                TextInput("Name", &m_dialogName, 320.0f);
            },
            "Rename", "", "Cancel");
        if (r == DialogResult::Primary) {
            m_statusText = "Renamed to: " + m_dialogName;
        } else if (r == DialogResult::Close) {
            m_statusText = "Rename cancelled";
        }
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- TeachingTip ---
    Label("TeachingTip", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (Button("Feature button", Icons::Lightbulb)) {
        m_teachingTipOpen = true; // dispara la coachmark a demanda
    }
    Rect tipTarget(ctx->lastItemPos, ctx->lastItemSize);
    if (TeachingTip("demo_teaching_tip", tipTarget, "New feature!",
                    "This button now does something amazing. Try it out.",
                    "Try it", &m_teachingTipOpen)) {
        m_statusText = "TeachingTip acknowledged";
    }

    // TODO brief 17/18: SelectableText, HyperlinkButton, AutoSuggestBox,
    // TokenizingTextBox, PasswordBox, MarkdownView not integrated yet.
}

// --- Tab 7: Feedback & Status (brief 15) ------------------------------------

void App::BuildFeedback() {
    // --- InfoBar (4 severities) ---
    Label("InfoBar", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    InfoBar("ib_info", InfoSeverity::Informational, "Heads up",
            "This is an informational message with extra context.", false);
    Spacing(4);
    InfoBar("ib_success", InfoSeverity::Success, "Saved",
            "Your changes were saved successfully.", true);
    Spacing(4);
    InfoBar("ib_warning", InfoSeverity::Warning, "Low disk space",
            "You are running low on storage. Consider freeing some space.",
            true, "Manage");
    Spacing(4);
    InfoBar("ib_error", InfoSeverity::Error, "Upload failed",
            "The file could not be uploaded. Check your connection and retry.",
            true, "Retry");

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Toast ---
    Label("Toast notifications", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Horizontal(8.0f, [&] {
        if (Button("Info toast")) {
            ToastOptions opts;
            opts.severity = InfoSeverity::Informational;
            opts.durationSec = 4.0f;
            ShowToast("Notification", "An informational toast (#" +
                      std::to_string(++m_toastCounter) + ")", opts);
        }
        if (Button("Success toast")) {
            ToastOptions opts;
            opts.severity = InfoSeverity::Success;
            ShowToast("Done", "Operation completed successfully.", opts);
        }
        if (Button("Action toast")) {
            ToastOptions opts;
            opts.severity = InfoSeverity::Warning;
            opts.durationSec = 6.0f;
            opts.actionText = "Undo";
            opts.onAction = [this]() { m_statusText = "Toast action: Undo"; };
            ShowToast("Item deleted", "The item was moved to trash.", opts);
        }
    });
    Label("Toasts stack in the lower-right corner (RenderToasts in the loop).",
          std::nullopt, TypographyStyle::Caption, true);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- ProgressRing ---
    Label("ProgressRing", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Horizontal(24.0f, [&] {
        Vertical(2.0f, [&] {
            Label("Determinate", std::nullopt, TypographyStyle::Caption);
            ProgressRing("pr_determinate", 48.0f, m_progressValue);
        });
        Vertical(2.0f, [&] {
            Label("Indeterminate", std::nullopt, TypographyStyle::Caption);
            ProgressRing("pr_indeterminate", 48.0f, -1.0f);
        });
    });

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Badge ---
    Label("Badge", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Horizontal(40.0f, [&] {
        IconLabel(Icons::Bell, 28.0f);
        Badge(7);
        IconLabel(Icons::Mail, 28.0f);
        Badge(128); // shows "99+"
        IconLabel(Icons::User, 28.0f);
        Badge(0, true); // dot only
    });

    Spacing(16);
    Separator();
    Spacing(8);

    // --- Skeleton ---
    Label("Skeleton placeholders", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Horizontal(12.0f, [&] {
        Skeleton(Vec2(64, 64), 8.0f); // avatar block
        Vertical(6.0f, [&] {
            SkeletonText(3, 16.0f, 0.6f);
        });
    });
}

// --- Tab 8: Collections (brief 16) ------------------------------------------

void App::BuildCollections() {
    // --- GridView ---
    Label("GridView", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    GridView("demo_gridview", 30, Vec2(110, 80), [](int index) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Tile %d", index + 1);
        if (ScopedPanel sc_tile{std::string("gv_tile_") + std::to_string(index), Vec2(0, 0)}) {
            Label(buf, std::nullopt, TypographyStyle::Caption);
        }
    }, 8.0f, 0.0f);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- DataGrid (one editable column) ---
    Label("DataGrid", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    std::vector<DataColumn> cols;
    {
        DataColumn c0; c0.header = "Name";   c0.width = 120.0f; c0.editable = true;  c0.type = DataColumn::Type::Text;   cols.push_back(c0);
        DataColumn c1; c1.header = "Team";   c1.width = 130.0f; c1.editable = false; c1.type = DataColumn::Type::Choice; c1.choices = {"Engineering","Design","Marketing","Sales","Support"}; cols.push_back(c1);
        DataColumn c2; c2.header = "Score";  c2.width = 80.0f;  c2.editable = true;  c2.type = DataColumn::Type::Number; cols.push_back(c2);
        DataColumn c3; c3.header = "Active"; c3.width = 70.0f;  c3.editable = true;  c3.type = DataColumn::Type::Bool;   cols.push_back(c3);
    }
    DataGridResult dgr = DataGrid(
        "demo_datagrid", cols, (int)m_gridRows.size(),
        [this](int row, int col) -> std::string {
            if (row >= 0 && row < (int)m_gridRows.size() &&
                col >= 0 && col < (int)m_gridRows[row].size())
                return m_gridRows[row][col];
            return "";
        },
        [this](int row, int col, const std::string& newVal) {
            if (row >= 0 && row < (int)m_gridRows.size() &&
                col >= 0 && col < (int)m_gridRows[row].size())
                m_gridRows[row][col] = newVal;
        });
    if (dgr.editedRow >= 0) {
        m_statusText = "DataGrid edited row " + std::to_string(dgr.editedRow) +
                       " col " + std::to_string(dgr.editedCol);
    }

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Pagination ---
    Label("Pagination", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    int page = Pagination("demo_pagination", 42, &m_paginationPage);
    Label("Current page: " + std::to_string(page + 1) + " of 42",
          std::nullopt, TypographyStyle::Caption);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- ExpanderList (accordion) ---
    Label("ExpanderList (accordion)", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    static const char* faqQ[] = {"What is FluentGUI?", "Is it cross-platform?",
                                 "Which backends?", "How is licensing?"};
    static const char* faqA[] = {
        "An immediate-mode UI library with Fluent Design styling.",
        "Yes, it runs on Windows, Linux and macOS via SDL3.",
        "OpenGL and Vulkan are both supported.",
        "See the repository for license details."};
    ExpanderList("demo_expanderlist", 4,
                 [](int i) { return std::string(faqQ[i]); },
                 [](int i) { LabelWrapped(faqA[i], 480.0f); },
                 true);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- FlipView ---
    Label("FlipView / Carousel", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    int flip = FlipView("demo_flipview", 4, [](int index) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Slide %d / 4", index + 1);
        if (ScopedPanel sc_flip{std::string("flip_page_") + std::to_string(index), Vec2(420, 160)}) {
            Label(buf, std::nullopt, TypographyStyle::Title);
            Spacing(4);
            Label("Swipe with the arrows or dots below.");
        }
    }, &m_flipIndex);
    Label("Page index: " + std::to_string(flip), std::nullopt, TypographyStyle::Caption);
}

// --- Tab 9: Layout primitives (brief 19) ------------------------------------

void App::BuildLayout() {
    // --- WrapPanel ---
    Label("WrapPanel (resize the window to reflow)", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    WrapPanel("demo_wrap", 8.0f, 8.0f, [&] {
        static const char* chips[] = {"Design", "Engineering", "Marketing", "Sales",
                                      "Support", "Finance", "Legal", "Research",
                                      "Operations", "Product", "Security", "QA"};
        for (int i = 0; i < 12; i++) {
            Button(chips[i], ButtonSize::Small);
        }
    });

    Spacing(12);
    Separator();
    Spacing(8);

    // --- UniformGrid ---
    Label("UniformGrid (4 columns)", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    UniformGrid("demo_uniform", 4, 8.0f, [&] {
        for (int i = 0; i < 8; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "Cell %d", i + 1);
            if (ScopedPanel sc_cell{std::string("ug_cell_") + std::to_string(i), Vec2(0, 60)}) {
                Label(buf, std::nullopt, TypographyStyle::Caption);
            }
            if (i < 7)
                UniformGridNextCell();
        }
    });

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Breakpoint readout ---
    Label("Responsive Breakpoint", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Breakpoint bp = CurrentBreakpoint();
    const char* bpName = "Small";
    switch (bp) {
    case Breakpoint::Small:  bpName = "Small (< 640)";   break;
    case Breakpoint::Medium: bpName = "Medium (< 1008)"; break;
    case Breakpoint::Large:  bpName = "Large (< 1366)";  break;
    case Breakpoint::XLarge: bpName = "XLarge (>= 1366)"; break;
    }
    Label(std::string("Current breakpoint: ") + bpName, std::nullopt,
          TypographyStyle::BodyStrong);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- Canvas (absolute positioning) ---
    Label("Canvas (absolute positioning)", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Canvas("demo_canvas", Vec2(440, 200), [&] {
        CanvasChild(Vec2(20, 20), [this]() {
            if (Button("Top-left", Icons::Home))
                m_statusText = "Canvas: top-left";
        });
        CanvasChild(Vec2(280, 80), [this]() {
            if (Button("Center-right", Icons::Star))
                m_statusText = "Canvas: center-right";
        });
        CanvasChild(Vec2(120, 150), [this]() {
            Label("Free-floating label", std::nullopt, TypographyStyle::Caption);
        });
    });
}

// --- Tab 10: Navigation (brief 13) ------------------------------------------

void App::BuildNavigation() {
    // NOTA: TitleBar (window chrome) es el único widget sin ejemplo aquí porque es
    // chrome de ventana completa (barra de ancho total + caption buttons pegados al
    // borde derecho + drag/resize por hit-test), no un widget in-content. Su demo
    // real está en main.cpp con RUN_TITLEBAR_DEMO=1 (FluentApp borderless).

    // Sidebar navigation items with sub-items and a badge.
    std::vector<NavItem> navItems = {
        {"home",     "Home",     Icons::Home,     0, {}},
        {"mail",     "Mail",     Icons::Mail,     5, {}},
        {"docs",     "Documents", Icons::Folder,  0, {
            {"recent",  "Recent",    Icons::FileText, 0, {}},
            {"shared",  "Shared",    Icons::User,     2, {}},
        }},
        {"media",    "Media",    Icons::Image,    0, {}},
    };
    std::vector<NavItem> footerItems = {
        {"settings", "Settings", Icons::Settings, 0, {}},
    };

    Horizontal(12.0f, [&] {

    // Left: NavigationView. Returns the currently selected key.
    std::string sel = NavigationView("demo_nav", navItems, &m_selectedNavKey,
                                     NavDisplayMode::Expanded, footerItems);
    // Drive the page frame from the sidebar selection.
    if (sel != m_navFrame.current && !sel.empty()) {
        NavigateTo(m_navFrame, sel);
        m_statusText = "Navigated to: " + sel;
    }

    // Right: content area with CommandBar + Breadcrumb + NavFrame history.
    Vertical(8.0f, [&] {

    // CommandBar with primary + secondary (overflow) commands.
    std::vector<CommandItem> primaryCmds = {
        {"New",     Icons::Plus,     [this]() { m_statusText = "Cmd: New"; },     true, true},
        {"Save",    Icons::Save,     [this]() { m_statusText = "Cmd: Save"; },    true, true},
        {"Share",   Icons::Upload,   [this]() { m_statusText = "Cmd: Share"; },   true, true},
    };
    std::vector<CommandItem> secondaryCmds = {
        {"Settings", Icons::Settings, [this]() { m_statusText = "Cmd: Settings"; }, false, true},
        {"Help",     Icons::CircleHelp, [this]() { m_statusText = "Cmd: Help"; },   false, true},
    };
    CommandBar("demo_cmdbar", primaryCmds, secondaryCmds);

    Spacing(4);

    // BreadcrumbBar reflecting the current page.
    std::vector<std::string> crumbs = {"Workspace", "Library", m_navFrame.current};
    int clicked = BreadcrumbBar("demo_breadcrumb", crumbs);
    if (clicked >= 0) {
        m_statusText = "Breadcrumb #" + std::to_string(clicked) + " clicked";
    }

    Spacing(4);

    // NavFrame back/forward history controls.
    Horizontal(8.0f, [&] {
        if (Button("Back", Icons::ChevronRight)) {
            if (NavigateBack(m_navFrame)) {
                m_selectedNavKey = m_navFrame.current;
                m_statusText = "Back to: " + m_navFrame.current;
            }
        }
        if (Button("Forward", Icons::ChevronRight)) {
            if (NavigateForward(m_navFrame)) {
                m_selectedNavKey = m_navFrame.current;
                m_statusText = "Forward to: " + m_navFrame.current;
            }
        }
    });

    Spacing(8);

    // The "page" content itself.
    if (ScopedPanel sc_navPage{"nav_page_content", Vec2(0, 240)}) {
        Label("Page: " + m_navFrame.current, std::nullopt, TypographyStyle::Title);
        Spacing(6);
        Label("Back stack: " + std::to_string(m_navFrame.backStack.size()) +
              "   Forward stack: " + std::to_string(m_navFrame.forwardStack.size()),
              std::nullopt, TypographyStyle::Caption);
        Spacing(6);
        Label("Select an item in the sidebar to navigate. The frame keeps a "
              "back/forward history.");
    }

    });

    });
}

// --- Tab 11: Rich Text (brief 17) -------------------------------------------
// Showcases the rich-text / content widgets: SelectableText, HyperlinkButton,
// AutoSuggestBox, TokenizingTextBox, PasswordBox and MarkdownView.
// brief 18: i18n/RTL/a11y son de plataforma, no widgets de galería.
void App::BuildRichText() {
    // brief 31: ScopedVertical — el guard cierra el layout al salir de la función
    // (equivale al EndVertical del final), sin envolver todo el cuerpo en un lambda.
    ScopedVertical sc_richText{8.0f};

    // --- SelectableText -----------------------------------------------------
    Label("SelectableText (drag to select, Ctrl+C to copy)", std::nullopt,
          TypographyStyle::Subtitle);
    Spacing(4);
    SelectableText(
        "rt_sel",
        "FluentUI is a lightweight immediate-mode C++ GUI library with a Fluent "
        "Design look. This paragraph is read-only but fully selectable: press and "
        "drag with the mouse to highlight a range, double-click to select a word, "
        "triple-click to select the whole line, and use Ctrl+A then Ctrl+C to copy "
        "everything to the system clipboard. The text wraps to the available width.",
        0.0f, /*wrap=*/true);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- HyperlinkButton ----------------------------------------------------
    Label("HyperlinkButton", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    if (HyperlinkButton("Fluent Design documentation",
                        "https://learn.microsoft.com/windows/apps/design/"))
        m_statusText = "Opened Fluent Design docs";
    if (HyperlinkButton("Anthropic", "https://www.anthropic.com/"))
        m_statusText = "Opened anthropic.com";

    Spacing(12);
    Separator();
    Spacing(8);

    // --- AutoSuggestBox -----------------------------------------------------
    Label("AutoSuggestBox (type to filter)", std::nullopt,
          TypographyStyle::Subtitle);
    Spacing(4);
    auto fruitSuggestions = [](const std::string& query) {
        static const std::vector<std::string> kFruits = {
            "Apple", "Apricot", "Banana", "Blueberry", "Cherry", "Cranberry",
            "Grape", "Grapefruit", "Lemon", "Lime", "Mango", "Orange", "Papaya",
            "Peach", "Pear", "Pineapple", "Plum", "Raspberry", "Strawberry",
            "Watermelon"};
        std::vector<std::string> out;
        // Case-insensitive substring filter over the demo list.
        std::string q;
        q.reserve(query.size());
        for (char c : query) q += (char)std::tolower((unsigned char)c);
        for (const auto& f : kFruits) {
            std::string lf;
            lf.reserve(f.size());
            for (char c : f) lf += (char)std::tolower((unsigned char)c);
            if (q.empty() || lf.find(q) != std::string::npos)
                out.push_back(f);
        }
        return out;
    };
    std::string picked = AutoSuggestBox("rt_suggest", &m_autoSuggestText,
                                        fruitSuggestions, "Search fruit...");
    if (!picked.empty())
        m_statusText = "Picked suggestion: " + picked;
    Spacing(4);
    Label(m_autoSuggestText.empty()
              ? std::string("(nothing typed yet)")
              : std::string("Current text: ") + m_autoSuggestText,
          std::nullopt, TypographyStyle::Caption);

    Spacing(12);
    Separator();
    Spacing(8);

    // --- TokenizingTextBox --------------------------------------------------
    Label("TokenizingTextBox / Chips (Enter or comma to add, Backspace to remove)",
          std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    auto tagSuggestions = [](const std::string& query) {
        static const std::vector<std::string> kTags = {
            "C++", "Vulkan", "OpenGL", "FluentUI", "Rendering", "SDF", "Shaders",
            "Editor", "Tooling", "ImGui", "Win32", "SDL"};
        std::vector<std::string> out;
        std::string q;
        for (char c : query) q += (char)std::tolower((unsigned char)c);
        for (const auto& t : kTags) {
            std::string lt;
            for (char c : t) lt += (char)std::tolower((unsigned char)c);
            if (!q.empty() && lt.find(q) != std::string::npos)
                out.push_back(t);
        }
        return out;
    };
    if (TokenizingTextBox("rt_tokens", &m_tokens, "Add a tag...", tagSuggestions))
        m_statusText = "Tokens changed (" + std::to_string(m_tokens.size()) + ")";

    Spacing(12);
    Separator();
    Spacing(8);

    // --- PasswordBox --------------------------------------------------------
    Label("PasswordBox (click the eye to reveal)", std::nullopt,
          TypographyStyle::Subtitle);
    Spacing(4);
    if (PasswordBox("rt_password", &m_password, "Enter a password..."))
        m_statusText = "Password length: " + std::to_string(m_password.size());

    Spacing(12);
    Separator();
    Spacing(8);

    // --- MarkdownView -------------------------------------------------------
    Label("MarkdownView", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    static const std::string kMarkdown =
        "# Markdown demo\n"
        "\n"
        "FluentUI ships a small **Markdown** renderer that supports a useful\n"
        "*subset* of the syntax, including inline `code` spans.\n"
        "\n"
        "## Features\n"
        "\n"
        "- Headings (`#` .. `###`)\n"
        "- **Bold**, *italic* and `code`\n"
        "- Unordered lists and block quotes\n"
        "- Links like [Fluent docs](https://learn.microsoft.com/windows/apps/design/)\n"
        "\n"
        "> Block quotes are rendered with an accent bar.\n"
        "\n"
        "---\n"
        "\n"
        "That horizontal rule above closes the demo.\n";
    MarkdownView("rt_markdown", kMarkdown);
}

// --- Tab 13: Cards (brief 32) -----------------------------------------------

void App::BuildCards() {
    Label("Card (brief 32)", std::nullopt, TypographyStyle::Subtitle);
    Spacing(4);
    Label("Un solo Card parametrizado por CardConfig: 4 estilos x "
          "{estatica, clicable, seleccionable}. Sin widgets separados.",
          std::nullopt, TypographyStyle::Caption);
    Spacing(12);

    // --- 4 estilos (estaticas) ---
    Label("Estilos", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(6);
    static const char* kStyleNames[] = {"Elevated", "Outlined", "Filled", "Acrylic"};
    static const CardStyle kStyles[] = {CardStyle::Elevated, CardStyle::Outlined,
                                        CardStyle::Filled, CardStyle::Acrylic};
    Horizontal(12.0f, [&] {
        for (int i = 0; i < 4; ++i) {
            CardConfig cfg;
            cfg.style = kStyles[i];
            cfg.size = Vec2(170, 100);
            Card(std::string("style_card_") + std::to_string(i), cfg, [&] {
                Label(kStyleNames[i], std::nullopt, TypographyStyle::BodyStrong);
                Spacing(2);
                Label("Superficie estatica.", std::nullopt, TypographyStyle::Caption);
            });
        }
    });
    Spacing(16);

    // --- Clicable + auto-exclusion de un boton interno ---
    Label("Clicable + auto-exclusion", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(6);
    static std::string s_clickStatus = "-";
    Horizontal(12.0f, [&] {
        CardConfig cfg;
        cfg.style = CardStyle::Elevated;
        cfg.clickable = true;
        cfg.fitContent = true; // la card se ajusta al ancho de su contenido (shrink-to-fit)
        if (Card("click_card", cfg, [&] {
                Label("Card clicable", std::nullopt, TypographyStyle::BodyStrong);
                Spacing(2);
                // Label normal (ancho natural): con fitContent la card se ajusta a la
                // linea mas ancha, asi que el texto llena el ancho y el padding queda
                // simetrico sin hueco a la derecha. (No usar LabelWrapped con fitContent:
                // dependencia circular ancho<->wrap.)
                Label("El boton interno NO dispara la card.", std::nullopt,
                      TypographyStyle::Caption);
                Spacing(8);
                if (Button("Boton interno")) s_clickStatus = "boton interno (card NO activada)";
            })) {
            s_clickStatus = "card activada";
        }
        Vertical(4.0f, [&] {
            Label("Ultimo evento:", std::nullopt, TypographyStyle::Caption);
            Label(s_clickStatus, std::nullopt, TypographyStyle::Body);
        });
    });
    Spacing(16);

    // --- Seleccionable (elegir 1 de 3) ---
    Label("Seleccionable (elige 1 de 3)", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(6);
    static int s_selected = 0;
    Horizontal(12.0f, [&] {
        for (int i = 0; i < 3; ++i) {
            bool sel = (s_selected == i);
            CardConfig cfg;
            cfg.style = CardStyle::Outlined;
            cfg.selectable = true;
            cfg.size = Vec2(150, 90);
            BeginCard(std::string("sel_card_") + std::to_string(i), cfg, &sel);
            Label(std::string("Plan ") + std::to_string(i + 1), std::nullopt,
                  TypographyStyle::BodyStrong);
            Spacing(2);
            Label(sel ? "Seleccionado" : "Toca para elegir", std::nullopt,
                  TypographyStyle::Caption);
            EndCard();
            // Seleccion exclusiva a partir del estado toggled por EndCard.
            if (sel && s_selected != i) s_selected = i;
            else if (!sel && s_selected == i) s_selected = -1;
        }
    });
    Spacing(16);

    // --- One-shots ---
    Label("One-shots", std::nullopt, TypographyStyle::BodyStrong);
    Spacing(6);
    InfoCard("info_card_demo", "InfoCard",
             "Titulo + descripcion + icono opcional. Ideal para tiles de dashboard.",
             Icons::Info);
    Spacing(8);
    static bool s_settingToggle = true;
    SettingsCard("settings_card_demo", Icons::Settings, "Modo oscuro",
                 "Cambia el tema de la aplicacion",
                 [&] { ToggleSwitch("", &s_settingToggle); });
}

// --- Tab 13: Extras (widgets antes sin probar en la galería) -----------------

void App::BuildExtraWidgets() {
    constexpr float kAutoScale = 3.402823466e+38F; // FLT_MAX = escala automática

    if (ScopedTabView sc_extra{"extra_tabs", &m_extraTab, m_extraTabLabels,
                               Vec2(0, 420)}) {
        switch (m_extraTab) {
        case 0: {
            // --- Entrada: Drag*, ComboBox variantes, SegmentedControl, IconButton ---
            Label("Drag editors (arrastra en horizontal; doble-clic = teclado)",
                  std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            static float s_dragF = 42.0f;
            DragFloat("DragFloat", &s_dragF, 0.5f, 0.0f, 100.0f);
            static int s_dragI = 7;
            DragInt("DragInt", &s_dragI, 0.2f, 0, 100);
            static float s_vec3[3] = {1.0f, 2.0f, 3.0f};
            DragFloat3("Posicion (XYZ)", s_vec3, 0.1f);
            Spacing(12);

            Label("ComboBox: variantes", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            static int s_cbSearch = 0;
            ComboBoxSearchable("Searchable", &s_cbSearch, m_comboItems, 220.0f);
            Spacing(4);
            Label("Sin label (solo id):", std::nullopt, TypographyStyle::Caption);
            static int s_cbNoLabel = 1;
            ComboBoxNoLabel("extra_cbnl", &s_cbNoLabel, m_comboItems, 220.0f);
            Spacing(12);

            Label("SegmentedControl (una opcion activa)", std::nullopt,
                  TypographyStyle::Subtitle);
            Spacing(6);
            static int s_seg = 0;
            static std::vector<std::string> s_segOpts = {"Mover", "Rotar", "Escalar"};
            SegmentedControl("extra_seg", s_segOpts, &s_seg);
            Spacing(12);

            Label("IconButton (toolbar)", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            Horizontal(6.0f, [&] {
                if (IconButton(Icons::Search))   m_statusText = "IconButton: buscar";
                if (IconButton(Icons::Settings)) m_statusText = "IconButton: ajustes";
                if (IconButton(Icons::Info))     m_statusText = "IconButton: info";
            });
            break;
        }
        case 1: {
            // --- Pickers: fecha, hora, fecha+hora, color ---
            Label("Date / Time pickers", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            static DateTimeValue s_date = {2026, 7, 6, 14, 30, 0};
            DatePicker("Fecha", &s_date);
            Spacing(8);
            static DateTimeValue s_time = {2026, 7, 6, 9, 15, 0};
            TimePicker("Hora", &s_time);
            Spacing(8);
            static DateTimeValue s_dt = {2026, 7, 6, 18, 0, 0};
            DateTimePicker("Fecha y hora", &s_dt);
            Spacing(14);

            Label("ColorPicker (HSV + RGB + hex)", std::nullopt,
                  TypographyStyle::Subtitle);
            Spacing(6);
            static Color s_color = Color(0.2f, 0.6f, 1.0f, 1.0f);
            ColorPicker("Color de acento", &s_color);
            break;
        }
        case 2: {
            // --- Graficas: PlotLines, PlotHistogram, Sparkline ---
            static float s_plot[48];
            static bool s_plotInit = false;
            if (!s_plotInit) {
                for (int i = 0; i < 48; ++i)
                    s_plot[i] = std::sin(i * 0.35f) * 0.5f + 0.5f;
                s_plotInit = true;
            }
            Label("PlotLines", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            PlotLines("Seno", s_plot, 48, 0, "onda", kAutoScale, kAutoScale,
                      Vec2(380, 70));
            Spacing(14);
            Label("PlotHistogram", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            PlotHistogram("Barras", s_plot, 48, 0, "", kAutoScale, kAutoScale,
                          Vec2(380, 70));
            Spacing(14);
            Label("Sparkline (inline, sin ejes)", std::nullopt,
                  TypographyStyle::Subtitle);
            Spacing(6);
            Horizontal(8.0f, [&] {
                Label("CPU:", std::nullopt, TypographyStyle::Caption);
                Sparkline(s_plot, 48, Vec2(140, 22));
            });
            break;
        }
        case 3: {
            // --- Contenedores: CollapsingHeader, Grid, Toolbar, Table, StatusBar ---
            Label("CollapsingHeader (auto-indent, sin End)", std::nullopt,
                  TypographyStyle::Subtitle);
            Spacing(6);
            static bool s_ch = true;
            if (CollapsingHeader("Transform", &s_ch, Icons::Box)) {
                static float s_chScale = 1.0f;
                SliderFloat("Escala", &s_chScale, 0.0f, 2.0f, 220.0f);
                Label("Contenido indentado de la seccion.", std::nullopt,
                      TypographyStyle::Caption);
            }
            Spacing(12);

            Label("Grid (columnas fijas)", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            BeginGrid("extra_grid", 3, 0.0f);
            for (int i = 0; i < 6; ++i) {
                GridNextCell();
                Button("Celda " + std::to_string(i + 1));
            }
            EndGrid();
            Spacing(12);

            Label("Toolbar", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            BeginToolbar();
            Button("Archivo");
            Button("Editar");
            Button("Ver");
            EndToolbar();
            Spacing(12);

            Label("Table (ordenable / redimensionable + fila seleccionable)",
                  std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            static std::vector<TableColumn> s_cols = {
                {"Nombre", 150.0f, 40.0f, true, 0},
                {"Rol", 140.0f, 40.0f, true, 0},
                {"Nivel", 80.0f, 40.0f, true, 0}};
            static TableState s_tableState;
            static const char* s_rows[4][3] = {
                {"Alice", "Engineering", "98"}, {"Bob", "Design", "87"},
                {"Carol", "Marketing", "75"},   {"Dave", "Sales", "91"}};
            if (BeginTable("extra_table", s_cols, 4, Vec2(0, 170), &s_tableState)) {
                for (int r = 0; r < 4; ++r) {
                    TableNextRow();
                    TableRowSelectable(r);
                    TableSetCell(0); Label(s_rows[r][0]);
                    TableSetCell(1); Label(s_rows[r][1]);
                    TableSetCell(2); Label(s_rows[r][2]);
                }
                EndTable();
            }
            Spacing(12);

            Label("StatusBar", std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            BeginStatusBar("Listo");
            Label("Ln 1, Col 1", std::nullopt, TypographyStyle::Caption);
            EndStatusBar();
            break;
        }
        case 4: {
            // --- Arbol / Texto / Media / Responsive ---
            Label("TreeNodeMulti (Ctrl/Shift + clic = multi-seleccion)",
                  std::nullopt, TypographyStyle::Subtitle);
            Spacing(6);
            static std::vector<int> s_multiSel = {0};
            static bool s_treeOpen[4] = {false, false, false, false};
            if (BeginTreeView("extra_treemulti", Vec2(300, 150))) {
                TreeNodeMulti("tm0", "Objeto A", 0, &s_treeOpen[0], &s_multiSel);
                TreeNodeMulti("tm1", "Objeto B", 1, &s_treeOpen[1], &s_multiSel);
                TreeNodeMulti("tm2", "Objeto C", 2, &s_treeOpen[2], &s_multiSel);
                TreeNodeMulti("tm3", "Objeto D", 3, &s_treeOpen[3], &s_multiSel);
            }
            EndTreeView();
            Label("Seleccionados: " + std::to_string((int)s_multiSel.size()),
                  std::nullopt, TypographyStyle::Caption);
            Spacing(14);

            Label("LabelRich (markup inline)", std::nullopt,
                  TypographyStyle::Subtitle);
            Spacing(6);
            LabelRich("Texto con <b>negrita</b> y <i>cursiva</i> en una sola linea.");
            Spacing(14);

            Label("MediaCard (sin textura: solo contenido)", std::nullopt,
                  TypographyStyle::Subtitle);
            Spacing(6);
            MediaCard("extra_media", nullptr, Vec2(260, 120), [&] {
                Label("Titulo de la media", std::nullopt,
                      TypographyStyle::BodyStrong);
                Spacing(2);
                Label("Descripcion debajo de la imagen.", std::nullopt,
                      TypographyStyle::Caption);
            });
            Spacing(14);

            Label("Responsive: VisibleFrom / AdaptiveLayout", std::nullopt,
                  TypographyStyle::Subtitle);
            Spacing(6);
            VisibleFrom(Breakpoint::Medium, [&] {
                Label("Visible solo si el ancho es >= Medium (1008px).",
                      std::nullopt, TypographyStyle::Caption);
            });
            AdaptiveLayout([&](Breakpoint bp) {
                const char* n = bp == Breakpoint::Small    ? "Small"
                                : bp == Breakpoint::Medium ? "Medium"
                                : bp == Breakpoint::Large  ? "Large"
                                                           : "XLarge";
                Label(std::string("Breakpoint actual: ") + n, std::nullopt,
                      TypographyStyle::Caption);
            });
            break;
        }
        }
    }
}

// --- Tab 14: Laboratorio de nitidez de texto (brief 29) ---------------------
// A/B directo: el MISMO texto y tamaño renderizado por el camino BITMAP (hinted,
// buckets por tamaño) y por el camino MSDF (escalable, con el supersampling en cruz
// de la Fase E). El camino se fuerza moviendo el umbral bitmap por columna (seguro:
// DrawText resuelve el path con el umbral vigente en ese instante). Objetivo: decidir
// a ojo dónde poner el umbral y si el MSDF pequeño ya compite con el bitmap hinted.
void App::BuildTextSharpnessLab() {
    Label("Nitidez de texto — Bitmap (hinted) vs MSDF (Fase E)", std::nullopt,
          TypographyStyle::Subtitle);
    Spacing(4);
    Label("Mismo texto y tamano, lado a lado. Izquierda = camino BITMAP con hinting; "
          "derecha = camino MSDF con supersampling en cruz. Mira el peso de los fustes "
          "finos (las dos 't') y bordes limpios sin engorde por fase subpixel. El "
          "umbral real de la app es 28px. Recuerda: el texto NO usa dpiScale.",
          std::nullopt, TypographyStyle::Caption);
    Spacing(12);

    // ── Briefs 35-A / 35-B / 35-C: controles en caliente ────────────────────
    // Todo lo que mueven estos controles son uniforms / parametros por batch, asi
    // que el render se actualiza en el frame siguiente SIN recrear ningun pipeline.
    {
        Renderer::TextQuality q = ctx->renderer.GetTextQuality();
        bool changed = false;

        Label("Ajuste en caliente (35-A curva, 35-B subpixel, 35-C rejilla)",
              std::nullopt, TypographyStyle::BodyStrong);
        Spacing(4);

        changed |= Checkbox("Curva gamma/contraste", &q.contrastEnabled);
        changed |= SliderFloat("Gamma claro-sobre-oscuro", &q.gammaLightOnDark, 0.5f, 2.5f);
        changed |= SliderFloat("Gamma oscuro-sobre-claro", &q.gammaDarkOnLight, 0.5f, 2.5f);
        changed |= SliderFloat("Contraste", &q.contrast, 0.0f, 1.0f);

        const bool subAvail = ctx->renderer.GetBackend() &&
                              ctx->renderer.GetBackend()->Supports(RenderCap::SubpixelText);
        changed |= Checkbox("AA subpixel (ClearType)", &q.subpixelEnabled);
        Label(subAvail ? "  backend con dual-source: disponible"
                       : "  backend SIN dual-source: se usa escala de grises",
              std::nullopt, TypographyStyle::Caption);
        changed |= Checkbox("Franjas BGR (en vez de RGB)", &q.bgrStripes);
        changed |= SliderFloat("Reduccion de fringing", &q.fringe, 0.0f, 1.0f);
        changed |= Checkbox("Grid-fit vertical", &q.verticalGridFit);

        if (changed) ctx->renderer.SetTextQuality(q);
        Spacing(10);
        Separator();
        Spacing(10);
    }

    const float sizes[] = {11.0f, 13.0f, 16.0f, 18.0f, 20.0f, 28.0f, 40.0f};
    const std::string sample = "Buttons";
    const Color txt = ctx->style.GetTextStyle(TypographyStyle::Body).color;
    const Color dim = ctx->style.GetTextStyle(TypographyStyle::Caption).color;

    const float savedThreshold = ctx->renderer.GetTextBitmapThreshold();

    const float tagW = 60.0f;    // columna de la etiqueta "40px"
    const float colW = 200.0f;   // ancho por columna de muestra

    // Encabezados de columna
    {
        Vec2 p = ctx->cursorPos;
        ctx->renderer.DrawText(Vec2(p.x,                p.y), "tamano",          dim, 12.0f);
        ctx->renderer.DrawText(Vec2(p.x + tagW,         p.y), "BITMAP (hinted)", dim, 12.0f);
        ctx->renderer.DrawText(Vec2(p.x + tagW + colW,  p.y), "MSDF (Fase E)",   dim, 12.0f);
    }
    Spacing(22.0f);
    Separator();
    Spacing(10.0f);

    for (float sz : sizes) {
        const Vec2 p = ctx->cursorPos;
        const float tagY = p.y + (sz > 12.0f ? (sz - 12.0f) * 0.4f : 0.0f); // centrar opticamente
        char tag[16];
        snprintf(tag, sizeof(tag), "%.0fpx", sz);
        ctx->renderer.DrawText(Vec2(p.x, tagY), tag, dim, 12.0f);

        // Columna izquierda: forzar BITMAP (umbral muy alto)
        ctx->renderer.SetTextBitmapThreshold(1000.0f);
        ctx->renderer.DrawText(Vec2(p.x + tagW, p.y), sample, txt, sz);

        // Columna derecha: forzar MSDF (umbral 0)
        ctx->renderer.SetTextBitmapThreshold(0.0f);
        ctx->renderer.DrawText(Vec2(p.x + tagW + colW, p.y), sample, txt, sz);

        ctx->renderer.SetTextBitmapThreshold(savedThreshold);
        Spacing(sz + 14.0f);
    }

    Spacing(6.0f);
    Separator();
    Spacing(10.0f);

    // Referencia: el mismo texto por el camino AUTOMATICO real (umbral vigente).
    // La etiqueta muestra que path elige de verdad la app en cada tamano.
    Label("Referencia — camino AUTO real de la app", std::nullopt,
          TypographyStyle::BodyStrong);
    Spacing(6.0f);
    for (float sz : sizes) {
        const Vec2 p = ctx->cursorPos;
        const bool bmp = ctx->renderer.WillUseBitmapText(sz);
        const float tagY = p.y + (sz > 12.0f ? (sz - 12.0f) * 0.4f : 0.0f);
        char tag[24];
        snprintf(tag, sizeof(tag), "%.0fpx %s", sz, bmp ? "bitmap" : "msdf");
        ctx->renderer.DrawText(Vec2(p.x, tagY), tag, dim, 12.0f);
        ctx->renderer.DrawText(Vec2(p.x + tagW + 40.0f, p.y), sample, txt, sz);
        Spacing(sz + 14.0f);
    }

    // Defensivo: garantizar que el umbral queda como estaba.
    ctx->renderer.SetTextBitmapThreshold(savedThreshold);
}
