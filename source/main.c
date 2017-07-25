#include <stdio.h>
#include <3ds.h>
#include <string.h>
#include <citro3d.h>
#include <stdlib.h>

#include "mcu.h"
#include "vshader_shbin.h" //Generated by the build process using the .pica file.
#include "lodepng.h"
#include "RTChanger_png.h"



#define CLEAR_COLOR 0x000000FF
//Code from ctrulib which allows for the 3DS to transfer the rendered image to the framebuffer.
#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
//Converts the textures to tiled format.
#define TEXTURE_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define NUM_SPRITES 1

#define MAX_SPRITES   1600
#define MAX_IMMEDIATE 500

typedef struct {
    float x,y;          // Screen coordinates.
    float dx, dy;       // Velocity.
    int image;
}Sprite;

Sprite sprites[NUM_SPRITES];

struct { float left, right, top, bottom; } images[4] = {
    {0.0f, 1.0f, 0.0f, 1.0f},
    {0.0f, 1.0f, 0.0f, 1.0f},
    {0.0f, 1.0f, 0.0f, 1.0f},
    {0.0f, 1.0f, 0.0f, 1.0f},
};

typedef struct  
{
    u8 seconds;
    u8 minute;
    u8 hour;
    u8 something; //Unused offset.
    u8 day;
    u8 month;
    u8 year;
} RTC;

const int cursorOffset[] = { //Sets a cursor below the selected value.
    19,
    16,
    13,
    0, //Unused offset.
    10,
    7,
    3
 };

void bcdfix(u8* wat)
{
    if((*wat & 0xF) == 0xF) *wat -= 6;
    if((*wat & 0xF) == 0xA) *wat += 6;
}

static void drawSpriteImmediate( size_t idx, int x, int y, int width, int height, int image ) {
    float left = images[image].left;
    float right = images[image].right;
    float top = images[image].top;
    float bottom = images[image].bottom;
    
    if (idx > MAX_IMMEDIATE)
        return;
    
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);    // Draws a textured quad.
        C3D_ImmSendAttrib(x, y, 0.5f, 0.0f); // v0=position
        C3D_ImmSendAttrib( left, top, 0.0f, 0.0f);
        C3D_ImmSendAttrib(x, y+height, 0.5f, 0.0f);
        C3D_ImmSendAttrib( left, bottom, 0.0f, 0.0f);
        C3D_ImmSendAttrib(x+width, y, 0.5f, 0.0f);
        C3D_ImmSendAttrib( right, top, 0.0f, 0.0f);
        C3D_ImmSendAttrib(x+width, y+height, 0.5f, 0.0f);
        C3D_ImmSendAttrib( right, bottom, 0.0f, 0.0f);
    C3D_ImmDrawEnd();
}

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection;
static C3D_Mtx projection;
static C3D_Tex spritesheet_tex;

static size_t numSprites = 1;
void (*drawSprite)(size_t,int,int,int,int,int) = drawSpriteImmediate;

Result initServices(PrintConsole topScreen, C3D_RenderTarget* target){ //Initializes the services.
    int i;
    
    vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&program);
    
    gfxInit(GSP_RGB565_OES, GSP_BGR8_OES, false); //Inits both screens.
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    
    C3D_RenderTargetSetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    
    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0=position
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2); // v2=texcoord
    
    Mtx_OrthoTilt(&projection, 0.0, 320.0, 240.0, 0.0, 0.0, 1.0, true); //Computes the projection matrix. Also uses the screen coordinates.
    
    unsigned char* image;
    unsigned width, height;
    
    lodepng_decode32(&image, &width, &height, RTChanger_png, RTChanger_png_size);
    
    u8 *gpusrc = linearAlloc(width*height*4);
    u8* src=image; u8 *dst=gpusrc;
    
    for(int i = 0; i<width*height; i++) { //Converting the big endian RGBA values from lodepng.
        int r = *src++;
        int g = *src++;
        int b = *src++;
        int a = *src++;
        *dst++ = a;
        *dst++ = b;
        *dst++ = g;
        *dst++ = r;
    }
    
    GSPGPU_FlushDataCache(gpusrc, width*height*4);           //Ensures the 'banner.png' is in physical RAM.
    
    C3D_TexInit(&spritesheet_tex, width, height, GPU_RGBA8); //Loads the texture and bind it to the first texture unit.
    //Converts the image to 3DS tiled texture format.
    C3D_SafeDisplayTransfer ((u32*)gpusrc, GX_BUFFER_DIM(width,height), (u32*)spritesheet_tex.data, GX_BUFFER_DIM(width,height), TEXTURE_TRANSFER_FLAGS);
    gspWaitForPPF();
    C3D_TexSetFilter(&spritesheet_tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexBind(0, &spritesheet_tex);
    free(image);
    linearFree(gpusrc);
	
    consoleInit(GFX_TOP, &topScreen);
    consoleSelect(&topScreen);
    Result res = mcuInit();
    return res;
}

static void moveSprites() {

    int i;

    for(i = 0; i < numSprites; i++) {
        sprites[i].x += sprites[i].dx;
        sprites[i].y += sprites[i].dy;

        //check for collision with the screen boundaries
        if(sprites[i].x < 1 || sprites[i].x > (400-32))
            sprites[i].dx = -sprites[i].dx;

        if(sprites[i].y < 1 || sprites[i].y > (240-32))
            sprites[i].dy = -sprites[i].dy;
    }
}

void deinitServices(){
    C3D_Fini();
    mcuExit();
    gfxExit();
}

static void sceneExit(void) {
    shaderProgramFree(&program); //Frees the shader program.
    DVLB_Free(vshader_dvlb);
}

void mcuFailure(){
    printf("\n\nPress any key to exit...");
    while (aptMainLoop())
    {
        hidScanInput();
        if(hidKeysDown())
        {
            sceneExit();
            deinitServices();
            break;
        }
        gspWaitForVBlank();
    }
    return;
}

static void sceneRender(void) {
    size_t i;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection); //Updates the uniforms.
    for(i = 0; i < NUM_SPRITES; i++) {
        drawSprite(i, 0, 0, 32, 32, sprites[i].image);
    }
}

int main()
{
    u32 kDown = 0;
    u32 kHeld = 0;
    u32 kUp = 0;
    
    hidScanInput();               //Scans for input.
    kDown = hidKeysDown();        //Detects if the A button was pressed.
    kHeld = hidKeysHeld();        //Detects if the A button was held.
    kUp = hidKeysUp();            //Detects if the A button was just released.
    while(kDown & KEY_A) 
    {
    PrintConsole topScreen;
    C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 320, GPU_RB_RGB8, 0);
    Result res = initServices(topScreen, target);
    
    if(res < 0)
    {
        printf("Failed to init MCU: %08X\n", res);
        puts("This .3DSX was likely opened without    Luma3DS or a SM patch.");
        puts("\x1b[30;41mYou cannot use this application without Luma3DS and Boot9Strap.\x1b[0m");
        puts("If you have Luma3DS 8.0 and up, just    ignore the above message and patch SM.  Restart the application afterwards.");
        puts("If you are confused, please visit my    GitHub and view the README.\n \n \n");
        puts("\x1b[36mhttps://www.github.com/Storm-Eagle20/RTChanger\x1b[0m");
        mcuFailure();
        return -1;
    }
    
    RTC mcurtc;
    mcuReadRegister(0x30, &mcurtc, 7);
    RTC rtctime;
    
    u32 kDown = 0;
    u32 kHeld = 0;
    u32 kUp = 0;
    
    u8* buf = &rtctime;
    u8 offs = 0;
    
    while (aptMainLoop()) //Detects the user input.
    {   
        printf ("\x1b[0;0H");
        puts ("Here you can change your time. Changing backwards is not recommended.");
        puts ("Change your time by however you may need.");
        puts ("The format is year, month, day, then hours,        minutes, and seconds.");
        puts ("When you are done setting the Raw RTC, press A to save the changes. \n");
        
        hidScanInput();               //Scans for input.
        kDown = hidKeysDown();        //Detects if the A button was pressed.
        kHeld = hidKeysHeld();        //Detects if the A button was held.
        kUp = hidKeysUp();            //Detects if the A button was just released.
        
        if(kHeld & KEY_START) break;  //User can choose to continue or return to the Home Menu.  
        
        if(kDown & (KEY_UP))          //Detects if the UP D-PAD button was pressed.
        {    
            buf[offs]++; //Makes an offset increasing the original value by one.
            switch(offs)
            {   
                case 0:  //seconds
                case 1:  //minutes
                    break;
                    
                case 2:  //hours
                    break;
                    
                case 4:  //days
                    break;
                    
                case 5:  //months
                    break;
                    
                case 6:  //years
                    break;
            }       
        }
        if(kDown & (KEY_DOWN))       //Detects if the UP D-PAD button was pressed.
        {    
            buf[offs]--; //Makes an offset decreasing the original value by one.
            switch(offs)
            {
                case 0:  //seconds
                case 1:  //minutes
                    break;
                    
                case 2:  //hours
                    break;
                    
                case 4:  //days
                    break;
                    
                case 5:  //months
                    break;
                    
                case 6:  //years
                    break;
            }
        }
        if(kDown & KEY_LEFT)
        {
            if(offs == 2) offs = 4;
            else if(offs < 6) offs++;
        }
        if(kDown & KEY_RIGHT)
        {
            if(offs == 4) offs = 2;
            else if(offs) offs--;
        }
        
        if(kDown & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) //The code for allowing the user to change the time.
        {
            bcdfix(buf + offs);
            printf("20%02X/%02X/%02X %02X:%02X:%02X\n", buf[6], buf[5], buf[4], buf[2], buf[1], buf[0]);
            printf("%*s\e[0K", cursorOffset[offs], "^^"); //The cursor.
        }
        if(kDown & KEY_A) //Allows the user to save the changes. Not implemented yet.
        {
        }
        
        moveSprites();
        
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW); //Renders the scene.
        C3D_FrameDrawOn(target);
        sceneRender();
        C3D_FrameEnd(0);
        
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
                                                            }
    sceneExit();
    
    return 0;
}
