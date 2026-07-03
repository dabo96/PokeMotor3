#include <glad/glad.h>
#include <FluentUI/API.h>

#define WINDOW_WIDTH 1920
#define WINDOW_HEIGHT 1080

using namespace FluentUI;

uint64_t last_time = 0;
int cursor = 0;
int coordinates = 0;
float gridSize = 0.0f;
int layer = 0;
int camera = 0;
int activeTab = 0;

static bool open = true;
static bool open1 = false;
static bool open2 = false;
static bool open3 = false;
static bool open4 = false;
static bool open5 = false;

std::string value = "";

static TableState state;
//state.frozenColumns = 1;

void Header(){
    //MenuBar
    BeginMenuBar();
        if(BeginMenu("File", true))
        {
            if(MenuItem("Prueba", true)){
                std::cout << "presione el menu" << std::endl;
            }
            EndMenu();
        }
        if(BeginMenu("Edit", true))
        {
            if(MenuItem("Prueba", true)){
                std::cout << "presione el menu" << std::endl;
            }
            EndMenu();
        }
        if(BeginMenu("View", true))
        {
            if(MenuItem("Prueba", true)){
                std::cout << "presione el menu" << std::endl;
            }
            EndMenu();
        }
        if(BeginMenu("Scene", true))
        {
            if(MenuItem("Prueba", true)){
                std::cout << "presione el menu" << std::endl;
            }
            EndMenu();
        }
        if(BeginMenu("Build", true))
        {
            if(MenuItem("Prueba", true)){
                std::cout << "presione el menu" << std::endl;
            }
            EndMenu();
        }
        if(BeginMenu("Window", true))
        {
            if(MenuItem("Prueba", true)){
                std::cout << "presione el menu" << std::endl;
            }
            EndMenu();
        }
        if(BeginMenu("Help", true))
        {
            if(MenuItem("Prueba", true)){
                std::cout << "presione el menu" << std::endl;
            }
            EndMenu();
        }
    EndMenuBar();

    Spacing(1.0f);

    BeginToolbar();

        SegmentedControl("Cursor",
        std::vector<std::pair<std::string, uint32_t>>{
            {"", Icons::MousePointer}, {"", Icons::Move},
            {"", Icons::RotateN3D}, {"", Icons::ScaleN3D}
        }, &cursor);
        
        Separator();

        SegmentedControl("Coordinates",
        std::vector<std::string>{
            "Local", "World"
        },
        &coordinates);

        Separator();

        SetNextConstraints(FixedSize(30.0f, 26.0f));
        if(IconButton(Icons::GridN2XN2)){

        }
        SetNextConstraints(FixedSize(50.0f, 26.0f));
        DragFloat("", &gridSize, 0.1f, 0.0f, 100.0f);

        Separator();

        Label("Layer: ");
        SetNextConstraints(FixedSize(90.0f, 26.0f));
        ComboBoxNoLabel("Layer", &layer, std::vector<std::string>{"Default"});

        Separator();

        Label("Camera");
        //SetNextConstraints(FixedSize(90.0f, 26.0f));
        ComboBoxNoLabel("Camera", &camera, std::vector<std::string>{"Editor Perspective"});
    EndToolbar();
}

void leftPanel(){
    if(BeginPanel("Hierarchy", Vec2{280, WINDOW_HEIGHT - 60.0f}))
    {
        TextInput("", &value, 200, false, Vec2{0,0}, "Search Entity");

        BeginTreeView("Hierarchy", {260, 640});
        if(TreeNode("world", "World", Icons::Folder, &open))
        {
            TreeNodePush();
            TreeNode("dir light", "Directional Light", Icons::Sun);
            TreeNode("skybox", "Skybox", Icons::Sunset);
            TreeNodePop();
        }
    
        if(TreeNode("environment", "Environment", Icons::Folder, &open1))
        {
            TreeNodePush();
            TreeNode("terrain", "Terrain", Icons::Sun);
            if(TreeNode("forest", "Forest", Icons::Trees, &open2))
            {
                TreeNodePush();
                TreeNode("terrain", "Terrain", Icons::Sun);
                TreeNodePop();
            }
            TreeNodePop();
        }
        
        EndTreeView();
    }
    
    EndPanel();
}

void tabAssets(){
    BeginTreeView("Hierarchy", {260, 640});
        if(TreeNode("assets", "Assets", Icons::Folder, &open3))
        {
            TreeNodePush();
            TreeNode("audio", "Audio", Icons::Folder);
            TreeNode("animations", "Animations", Icons::Folder);
            if(TreeNode("environment", "Environment", Icons::Folder, &open4))
            {
                TreeNodePush();
                TreeNode("terrain", "Terrain", Icons::Folder);
                if(TreeNode("forest", "Forests", Icons::Folder, &open5))
                {
                    TreeNodePush();
                    TreeNode("terrain", "Terrains", Icons::Folder);
                    TreeNodePop();
                }
                TreeNodePop();
            }
            TreeNodePop();
        }
        EndTreeView();
}

void bottomPanel(){
    BeginTabView("", &activeTab, std::vector<std::string>{"Assets", "Console", "Timeline"}, Vec2{1316, 210}, Vec2{280, WINDOW_HEIGHT-210});
    if(activeTab == 0){
        tabAssets();
    }else if(activeTab == 1){

    }else if(activeTab == 2){

    }

    EndTabView();
}

void UI(){

    Header();

    leftPanel();

    bottomPanel();
}

float calculate_dt() {
    uint64_t now = SDL_GetTicksNS(); // SDL3 usa nanosegundos para alta precisión
    
    // Si es la primera vez que se ejecuta, inicializamos last_time
    if (last_time == 0) {
        last_time = now;
        return 0.0f;
    }

    uint64_t delta_ns = now - last_time;
    last_time = now;

    // Convertimos nanosegundos a segundos (1 segundo = 1,000,000,000 ns)
    return (float)delta_ns / 1000000000.0f;
}

int main(int argc, char* argv[])
{
    SDL_Window* gWindow{ nullptr };
    SDL_GLContext gGlContext { nullptr };

    //Initialization flag
    bool success{ true };

    //Initialize SDL
    if( SDL_Init( SDL_INIT_VIDEO ) == false )
    {
        SDL_Log( "SDL could not initialize! SDL error: %s\n", SDL_GetError() );
        success = false;
    }
    else
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

        //Create window
        if( gWindow = SDL_CreateWindow( "SDL3 Tutorial: Hello SDL3", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_OPENGL); gWindow == nullptr )
        {
            SDL_Log( "Window could not be created! SDL error: %s\n", SDL_GetError() );
            success = false;
        }
        
        if(gGlContext = SDL_GL_CreateContext(gWindow); gGlContext == nullptr){
            SDL_Log( "Gl context could not be created! SDL error: %s\n", SDL_GetError() );
            success = false;
        }
        SDL_GL_MakeCurrent(gWindow, gGlContext);
    }

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        Log(LogLevel::Error, "OpenGL Error: Failed to initialize GLAD");
        return false;
    }
    
    FluentApp app(gWindow, gGlContext, true, true);

    //The quit flag
    bool quit{ false };

    //The event data
    SDL_Event e;
    SDL_zero( e );
    
    //The main loop
    while( quit == false )
    {
        float dt = calculate_dt();

        app.beginFrame(dt);

        //Get event data
        while( SDL_PollEvent( &e ) == true )
        {
            app.processEvent(e);
            //If event is quit type
            if( e.type == SDL_EVENT_QUIT )
            {
                //End the main loop
                quit = true;
            }
            
        }
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        UI();
        app.endFrame();

        //Update the surface
        SDL_GL_SwapWindow(gWindow);
    } 
    SDL_GL_DestroyContext(gGlContext);
    //Destroy window
    SDL_DestroyWindow( gWindow );
    gWindow = nullptr;

    //Quit SDL subsystems
    SDL_Quit();
    return 0;
}