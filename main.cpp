#include "library.h"
#include "mod/amlmod.h"
#include "mod/config.h"
#include "mod/logger.h"
#include "GTASA/common.h"
#include "shared/CEvents.h"
#include "shared/ScriptCommands.h"
#include "GTASA/CTimer.h"
#include "GTASA/CPools.h"
#include "GTASA/CCamera.h"
#include "funcs/Panda.h"
//#include "funcs/Shadow.h"
#include "GlossHook/include/Gloss.h"
#include "shared/Screen.h"
#include <iostream>
#include <cstdlib>
#include <ctime>

#define HOOKBW(_name, _fnAddr)                                    \
    CPatch::TrampolinesRedirectCall(SET_THUMB, _fnAddr, (void*)(&HookOf_##_name), (void**)(&_name), BW_THUMB32);
    
    
MYMODCFG(AdditionalVehicleFunctions, AVF, 1.1, PandaGaming)
NEEDGAME(com.rockstargames.gtasa)

using namespace plugin;

#include "isautils.h"
ISAUtils* sautils = nullptr;
#include "Ehfuncs/mgr.h"
#include "new_funcs/util.h"
#include "Ehfuncs/materials.h"
#include "Ehfuncs/indicators.h"
#include "Ehfuncs/dummy.h"
#include "new_funcs/common.h"
#include "Ehfuncs/lights.h"
#include "GTASA/CClock.h"
#include "GTASA/CModelInfo.h"

CCamera *camera;
bool* userPaused;
bool* codePaused;
uint32_t* m_snTimeInMillisecondsNonClipped;
uint32_t* m_snPreviousTimeInMillisecondsNonClipped;
float* ms_fTimeScale;

int nGameLoaded = -1;

CObject*    (*GetObjectFromRef)(int) = NULL;
CPed*       (*GetPedFromRef)(int) = NULL;
CVehicle*   (*GetVehicleFromRef)(int) = NULL;
bool        (*Get_Just_Switched_Status)(CCamera*) = NULL;

//#include "audiosystem.h"
IBASS* BASS = NULL;

static CSoundSystem soundsysLocal;
CSoundSystem* soundsys = &soundsysLocal;

VehicleMaterial::VehicleMaterial(CVehicle *vehicle, RpMaterial* material, eDummyPos pos) {
    Material = material;

    Texture = material->texture;
    TextureActive = material->texture;
    Pos = pos;
    Color = { material->color.red, material->color.green, material->color.blue, material->color.alpha };

    std::string name = std::string(Texture->name);
    
    int modelId = vehicle->m_nModelIndex;

    // Retrieve the model info using the model ID
    CBaseModelInfo* modelInfo = CModelInfo::ms_modelInfoPtrs[modelId];
    
    if (modelInfo)
    {
        // Get the DFF model name (without the .dff extension)
        const char* dffName = reinterpret_cast<const char*>(modelInfo->m_modelName);
        std::string modelName = std::string(dffName);
        
        RwTexture *pTexture = CTexLoader::LoadPNG("texture/avf/vehiclelights", std::string(modelName + name + "on").c_str());
            
        if (!pTexture) {
            pTexture = CTexLoader::LoadPNG("texture/avf/vehiclelights", std::string(modelName + name + "_on").c_str());
        }
        else if(!pTexture && name == "vehiclelights128")
        {
            pTexture = CTexLoader::LoadPNG("texture/avf/vehiclelights", "vehiclelightson128");
        }

        /*
        if (!pTexture && name == "vehiclelights128") {
            
        }*/
            
        if (pTexture) {
            TextureActive = pTexture;
        }
        
        /*
        if(modelName.c_str())
        {
            uintptr_t gta3 = sautils->GetTextureDB("gta3");
    
            sautils->RegisterTextureDB(gta3);
            
            RwTexture *pTexture = (RwTexture*)sautils->GetTexture(std::string(modelName + name + "on").c_str());
            
            if (!pTexture) {
                pTexture = (RwTexture*)sautils->GetTexture(std::string(modelName + name + "_on").c_str());
            }

            if (!pTexture) {
                pTexture = (RwTexture*)sautils->GetTexture(std::string(modelName + name + "vehiclelightson128").c_str());
            }
            
            if (pTexture) {
                TextureActive = pTexture;
            }
        
            sautils->UnregisterTextureDB(gta3);
        }*/
    }
};

void VehicleMaterials::Register(std::function<RpMaterial*(CVehicle*, RpMaterial*)> function) {
    functions.push_back(function);
};

void VehicleMaterials::RegisterRender(std::function<void(CVehicle*)> render) {
    renders.push_back(render);
};

void VehicleMaterials::RegisterDummy(std::function<void(CVehicle*, RwFrame*, std::string, bool)> function) {
    dummy.push_back(function);
};

void VehicleMaterials::OnModelSet(CVehicle* vehicle, int model) {
    currentVehicle = vehicle;

    RpClumpForAllAtomics(vehicle->m_pRwClump, [](RpAtomic* atomic, void* data) {
        if (!atomic->geometry)
            return atomic;

        RpGeometryForAllMaterials(atomic->geometry, [](RpMaterial* material, void* data) {
            if (!material || !material->texture)
                return material;

            if (materials[currentVehicle->m_nModelIndex].find(material) != materials[currentVehicle->m_nModelIndex].end())
                return material;

            for (auto& e: functions)
                e(currentVehicle, material);

            materials[currentVehicle->m_nModelIndex][material] = true;

            return material;
        }, atomic);

        return atomic;
    }, nullptr);

    /*if (dummies.find(currentVehicle->m_nModelIndex)  == dummies.end()|| dummies[currentVehicle->m_nModelIndex] == false) {
        dummies[currentVehicle->m_nModelIndex] = true;
    }*/

    VehicleMaterials::FindDummies(vehicle, (RwFrame*)vehicle->m_pRwClump->object.parent);
};

void VehicleMaterials::FindDummies(CVehicle* vehicle, RwFrame* frame, bool parent) {
    if (frame) {
        const std::string name = GetFrameNodeName(frame);

        if (RwFrame* nextFrame = frame->child) {
            FindDummies(vehicle, nextFrame, (RwFrameGetParent(frame))?(true):(false));
        }

        if (RwFrame* nextFrame = frame->next) {
            FindDummies(vehicle, nextFrame, parent);
        }

        for (auto e: dummy) {
            e(currentVehicle, frame, name, parent);
        }
    }
};

void VehicleMaterials::StoreMaterial(std::pair<unsigned int*, unsigned int> pair) {
    storedMaterials.push_back(pair);
};

void VehicleMaterials::RestoreMaterials() {
    for (auto& p : storedMaterials) {
        *p.first = p.second;
    }
    storedMaterials.clear();
};

void VehicleMaterials::OnRender(CVehicle* vehicle) {
    if (!renders.empty()) {
        for (auto e: renders) {
            e(vehicle);
        }
    }
};



enum etType : int
{
    TOUCH_UNPRESS = 0x1,
    TOUCH_PRESS = 0x2
    //TOUCH_MOVE = 0x3
};

CVector2D* TouchVec;
int tX, tY;
int tType, tFinger;

DECL_HOOK(void, TouchEvent, int type, int finger, int x, int y)
{
    tX = x;
    tY = y;
    tType = type;
    tFinger = finger;
    TouchEvent(type, finger, x, y);
};

bool IsWithinRadius(float checkX, float checkY, float radius) {
    //if (!TouchVec) return false;

    float dx = (float)tX - checkX;
    float dy = (float)tY - checkY;
    float distance = std::sqrt(dx * dx + dy * dy);

    return distance <= radius;
}

unsigned int touchStartTime = 0;
const unsigned int touchThreshold = 30; // Adjust this threshold as needed (in milliseconds)
bool istouchHandled = false;

bool IsTouched(float x, float y, float radius, int time) {
    if (IsWithinRadius(x, y, radius) && tType == TOUCH_PRESS) {
        if (!istouchHandled) {
            touchStartTime = CTimer::GetTimeInMS();
            istouchHandled = true;
        } else if (CTimer::GetTimeInMS() - touchStartTime < time) {
            return true;
        }
    } else {
        istouchHandled = false;
    }
    return false;
}

bool IsTouchedT(float x, float y, float radius)
{
    if(IsWithinRadius(x, y, radius) && tType == TOUCH_PRESS && tType != TOUCH_UNPRESS)
        return true;
    else if(IsWithinRadius(x, y, radius) && tType == TOUCH_UNPRESS && tType != TOUCH_PRESS || !IsWithinRadius(x, y, radius))
        return false;
    
}

CVector2D GetScreenCenter() {
    float centerX = RsGlobal.maximumWidth / 2.0f;
    float centerY = RsGlobal.maximumHeight / 2.0f;
    return CVector2D(centerX, centerY);
}

CVector2D GetScreenBottomCenter(float offset) {
    float centerX = RsGlobal.maximumWidth / 2.0f;
    float bottomY = RsGlobal.maximumHeight - offset;
    return CVector2D(centerX, bottomY);
}

CVector2D GetScreenTopCenter() {
    float centerX = RsGlobal.maximumWidth / 2.0f;
    float bottomY = RsGlobal.maximumHeight / 2.0f;
    bottomY /= 6.0f;
    return CVector2D(centerX, bottomY);
}

CSprite2d *bLeft, *bRight, *bHazard, *bAuto;

void InitTexture()
{
    bLeft = new CSprite2d;
    bRight = new CSprite2d;
    bHazard = new CSprite2d;
    bAuto = new CSprite2d;
    bLeft->m_pRwTexture = CTexLoader::LoadPNG("texture/avf", "SignalLeft");
    bRight->m_pRwTexture = CTexLoader::LoadPNG("texture/avf", "SignalRight");
    bHazard->m_pRwTexture = CTexLoader::LoadPNG("texture/avf", "SignalHazard");
    bAuto->m_pRwTexture = CTexLoader::LoadPNG("texture/avf", "Autopilot");
}

CRGBA lColor = CRGBA(50,50,50,255);
CRGBA rColor = CRGBA(50,50,50,255);
CRGBA hColor = CRGBA(50,50,50,255);
CRGBA aColor = CRGBA(50,50,50,255);

void DrawAutoButton()
{
    float slW = 110.0f;
    float slH = slW;
    float slWw = slW / 2.0f;
    float slHh = slH / 2.0f;
    
    CVector2D center = GetScreenBottomCenter(130.0f);
    
    float sHx = center.x;
    bAuto->Draw(sHx - slWw, center.y - slHh, slW, slH, aColor);
}

int touchA = 0;

int autoTouch()
{
    CVector2D center = GetScreenBottomCenter(130.0f);
    float radius = std::max(110.0f, 110.0f) / 2.0f;
    
    if (IsTouchedT(center.x, center.y, radius))
    {
        touchA = 1;
    }
    else
    {
        touchA = 0;
    }
    return touchA;
}

static bool TouchA = false;
bool aS = false;

bool GetAutoStatus()
{
    int tTouch = autoTouch();

    if (!TouchA)
    {
        if (tTouch == 1)
        {
            aS = !aS;
            TouchA = true;
        }
    }
    else
    {
        if (tTouch == 0)
        {
            TouchA = false;
        }
    }
    return aS;
}

#include "GTASA/CAutoPilot.h"
#include <fstream>
#include <string>
#include <map>

uintptr_t gMobileMenu;
uintptr_t ms_RadarTrace;
uintptr_t FrontEnd;

void SetSpeed(CVehicle* vehicle, float speed) {
    CAutoPilot& autopilot = vehicle->m_autoPilot;
    // Set the speed for the vehicle (speed is capped at 127 for cruise speed)
    autopilot.m_nCruiseSpeed = static_cast<uint8_t>(speed);
    // Optionally, set the max speed buffer (this allows setting higher speeds)
    autopilot.MaxSpeedBuffer = speed;
    // Ensure driving mode allows the vehicle to go toward the target
    autopilot.DrivingMode = DRIVINGSTYLE_AVOID_CARS;

}

bool autoState = false;

void AutoPilot(CVehicle *pVeh, CPed* player)
{
    int blipHndl = *(int*)(gMobileMenu + 72);
    CVector pos;
    if(blipHndl)
    {
       pos = *(CVector*)(ms_RadarTrace + *(uint16_t*)&blipHndl * 0x28 + 0x8);
    }
    
    uintptr_t vehStr = FindVehicle(-1, false);
    int hVeh = CPools::GetVehicleRef(pVeh);
    
    if(pos.x == 0.0f || pos.y == 0.0f || pos.z == 0.0f)
        return;
    
    if(pVeh->m_nVehicleSubClass == VEHICLE_AUTOMOBILE)
    {
        Command<Commands::CAR_GOTO_COORDINATES>(hVeh, pos.x, pos.y, pos.z);
        SetSpeed(pVeh, 25.0f);
    }
    
    CVector pPos = player->GetPosition();
    int pPlayer = CPools::GetPedRef(player);
    if(pPos.x == pos.x && pPos.y == pos.y)
    {
        Command<Commands::WARP_CHAR_INTO_CAR>(pPlayer, hVeh);
        autoState = false;
    }
}

bool offA = false;
bool offB = false;

struct MyDataAuto {
        unsigned int ModelId;
    };

static vector<MyDataAuto>& GetDataVectorAuto() {
    static vector<MyDataAuto> vec;
    return vec;
}

static void ReadSettingsFileAuto() {
    char path[0xFF];
    sprintf(path, "%sdata/autopilot.dat", aml->GetAndroidDataPath());
    ifstream stream(path);
    
    for (string line; getline(stream, line); ) {
        if (line[0] != ';' && line[0] != '#') {
            if (!line.compare("modelids")) {
                while (getline(stream, line) && line.compare("end")) {
                    if (line[0] != ';' && line[0] != '#') {
                        MyDataAuto entry;
                        if (sscanf(line.c_str(), "%d", &entry.ModelId) == 1)
                            GetDataVectorAuto().push_back(entry);
                    }
                }
            }
        }
    }
}

static MyDataAuto *GetDataInfoForModelAuto(unsigned int BaseModelId) {
        for (unsigned int i = 0; i < GetDataVectorAuto().size(); i++) {
            if (GetDataVectorAuto()[i].ModelId == BaseModelId)
                return &GetDataVectorAuto()[i];
        }
        return nullptr;
    }

void ProcessAutoPilot()
{
    static bool settingsLoaded = false;
    if (!settingsLoaded) {
        ReadSettingsFileAuto();
        settingsLoaded = true;
    }
    
    CPed *player = FindPlayerPed();
    if(!player->m_nPedFlags.bInVehicle)
        return;
        
    autoState = GetAutoStatus();
    
    CVehicle *pVeh = FindPlayerVehicle(-1, false);
   // CPed *player = FindPlayerPed();
    int playa = CPools::GetPedRef(player);
    int vehPtr = CPools::GetVehicleRef(pVeh);
    
    if(!pVeh)
        return;
    
    MyDataAuto *data = GetDataInfoForModelAuto(pVeh->m_nModelIndex);
    
    if(!data)
        return;
    
    if(autoState)
    {
        if(!offB)
        {
            AutoPilot(pVeh, player);
            offB = true;
        }
        aColor = CRGBA(150,150,150,255);
        offA = false;
    }
    else
    {
        if(!offA)
        {
           Command<Commands::WARP_CHAR_INTO_CAR>(playa, vehPtr);
           offA = true;
        }
        aColor = CRGBA(50,50,50,255);
        offB = false;
    }
}

void DrawSignalButton()
{
    //float sLy = 640.0f;
    float slW = 130.0f;
    float slH = slW;
    float slWw = slW / 2.0f;
    float slHh = slH / 2.0f;
    
    CVector2D center = GetScreenBottomCenter(50.0f);
   
    float sHx = center.x;
    bHazard->Draw(sHx - slWw, center.y - slHh, slW, slH, hColor);
    
    float sLx = center.x - 90.0f;
    bLeft->Draw(sLx - slWw, center.y - slHh, slW, slH, lColor);
    
    float sRx = center.x + 90.0f;
    bRight->Draw(sRx - slWw, center.y - slHh, slW, slH, rColor);
}

drawingEvent = []
{
    if(FindPlayerPed()->m_nPedFlags.bInVehicle)
    {
        if(cfg->Bind("Idicators", true, "Features")->GetBool())
        {
           DrawSignalButton();
        }

        if(cfg->Bind("AutoPilot", true, "Features")->GetBool())
        {
            DrawAutoButton();
        }
    }
};

enum eTouch : int
{
    off,
    left,
    right,
};

int touchS = 0;

CAudioStream *signal_click;

void* AEAudioHardware;
float (*GetEffectsMasterScalingFactor)(void*);
float (*GetMusicMasterScalingFactor)(void*);
float GetEffectsVolume()
{
        return GetEffectsMasterScalingFactor(AEAudioHardware);
}
float GetMusicVolume()
{
        return GetMusicMasterScalingFactor(AEAudioHardware);
}

int ShiftTouch()
{
    CVector2D center = GetScreenBottomCenter(50.0f);
    float radius = std::max(130.0f, 130.0f) / 2.0f;
    
    if (IsTouchedT(center.x - 90.0f, center.y, radius))
    {
        touchS = 1;
    }
    else if (IsTouchedT(center.x + 90.0f, center.y, radius))
    {
        touchS = 2;
    }
    else if(IsTouchedT(center.x, center.y, radius))
    {
        touchS = 3;
    }
    else
    {
        touchS = 0;
    }
    return touchS;
}

static bool touchHandled = false;

eLightState gS = eLightState::IndicatorNone;

eLightState GetLightsStatus()
{
    int tTouch = ShiftTouch();
    //CAudioStream *lever = LoadSong("turnlights/lever.mp3");
    
    static bool music = false;
    
    if(signal_click)
    {
        signal_click->SetType(SoundEffect);
        signal_click->SetVolume(GetEffectsVolume());
        music = true;
    }
    else
    {
        music = false;
    }

    if (!touchHandled)
    {
        if (tTouch == 1)
        {
            gS = (gS == eLightState::IndicatorLeft) ? eLightState::IndicatorNone : eLightState::IndicatorLeft;
            //PlaySong(lever, true);
            if(music)
            {
                signal_click->Play();
            }
            touchHandled = true;
        }
        else if (tTouch == 2)
        {
            gS = (gS == eLightState::IndicatorRight) ? eLightState::IndicatorNone : eLightState::IndicatorRight;
            //PlaySong(lever, true);
            if(music)
            {
                signal_click->Play();
            }
            touchHandled = true;
        }
        else if (tTouch == 3)
        {
            gS = (gS == eLightState::IndicatorBoth) ? eLightState::IndicatorNone : eLightState::IndicatorBoth;
            //PlaySong(lever, true);
            if(music)
            {
                signal_click->Play();
            }
            touchHandled = true;
        }
    }
    else
    {
        // Reset the touchHandled flag if no valid touch event is detected
        if (tTouch == 0)
        {
           // PlaySong(lever, false);
            /*if(music)
            {
                signal_click->Stop();
            }*/
            touchHandled = false;
        }
    }
    return gS;
}

inline bool IsNightTime() {
    return CClock::GetIsTimeInRange(20, 6);
}

inline unsigned int GetShadowAlphaForDayTime() {
    if (IsNightTime()) {
        return 210;
    } else {
        return 180;
    }
}

inline unsigned int GetCoronaAlphaForDayTime() {
    if (IsNightTime()) {
        return 180;
    } else {
        return 150;
    }
}

void DrawGlobalLight(CVehicle *pVeh, eDummyPos pos, CRGBA col) {
    if (pVeh->m_nVehicleSubClass == VEHICLE_AUTOMOBILE) {
        CAutomobile *ptr = reinterpret_cast<CAutomobile*>(pVeh);
        if ((pos == eDummyPos::FrontLeft && ptr->m_damageManager.GetLightStatus(eLights::LIGHT_FRONT_LEFT))
            || (pos == eDummyPos::FrontRight && ptr->m_damageManager.GetLightStatus(eLights::LIGHT_FRONT_RIGHT))
            || (pos == eDummyPos::RearLeft && ptr->m_damageManager.GetLightStatus(eLights::LIGHT_REAR_LEFT))
            || (pos == eDummyPos::RearRight && ptr->m_damageManager.GetLightStatus(eLights::LIGHT_REAR_RIGHT))) {
            return;
        }
    }

    int idx = (pos == eDummyPos::RearLeft) || (pos == eDummyPos::RearRight);
    bool leftSide = (pos == eDummyPos::RearLeft) || (pos == eDummyPos::FrontLeft);

    CVector posn =
        reinterpret_cast<CVehicleModelInfo*>(CModelInfo::ms_modelInfoPtrs[pVeh->m_nModelIndex])->m_pStructure->m_positions[idx];
    
    if (posn.x == 0.0f) posn.x = 0.15f;
    if (leftSide) posn.x *= -1.0f;
    int dummyId = static_cast<int>(idx) + (leftSide ? 0 : 2);
    float dummyAngle = (pos == eDummyPos::RearLeft || pos == eDummyPos::RearRight) ? 180.0f : 0.0f;
    //Common::RegisterShadow(pVeh, posn, col.r, col.g, col.b, GetShadowAlphaForDayTime(), dummyAngle, 0.0f, "indicator");
    Common::RegisterCoronaWithAngle(pVeh, posn, col.r, col.g, col.b, GetCoronaAlphaForDayTime(), dummyAngle, 0.3f, 0.3f);
}

CVector2D GetCarPathLinkPosition(CCarPathLinkAddress &address) {
    if (address.m_nAreaId != -1 && address.m_nCarPathLinkId != -1 && ThePaths.m_pPathNodes[address.m_nAreaId]) {
        return CVector2D(static_cast<float>(ThePaths.m_pNaviNodes[address.m_nAreaId][address.m_nCarPathLinkId].m_vecPosn.x) / 8.0f,
            static_cast<float>(ThePaths.m_pNaviNodes[address.m_nAreaId][address.m_nCarPathLinkId].m_vecPosn.y) / 8.0f);
    }
    return CVector2D(0.0f, 0.0f);
}

inline void DrawVehicleTurnlights(CVehicle *vehicle, eLightState lightsStatus) {
    if (lightsStatus == eLightState::IndicatorBoth || lightsStatus == eLightState::IndicatorRight) {
        DrawGlobalLight(vehicle, eDummyPos::FrontRight, {255, 128, 0, 0});
        DrawGlobalLight(vehicle, eDummyPos::RearRight, {255, 128, 0, 0});
    }
    if (lightsStatus == eLightState::IndicatorBoth || lightsStatus == eLightState::IndicatorLeft) {
        DrawGlobalLight(vehicle, eDummyPos::FrontLeft, {255, 128, 0, 0});
        DrawGlobalLight(vehicle, eDummyPos::RearLeft, {255, 128, 0, 0});
    }
}

inline float GetZAngleForPoint(CVector2D const &point) {
    float angle = CGeneral::GetATanOfXY(point.x, point.y) * 57.295776f - 90.0f;
    while (angle < 0.0f) angle += 360.0f;
    return angle;
}

inline bool IsBumperOrWingDamaged(CVehicle* pVeh, eDetachPart part) {
    if (pVeh->m_nVehicleSubClass == VEHICLE_AUTOMOBILE) {
        CAutomobile* ptr = reinterpret_cast<CAutomobile*>(pVeh);
        return ptr->m_damageManager.GetPanelStatus((ePanels)part);
    }
    return false;
}

// Shadows
unsigned int HEADLIGHT_SHADOW_ALPHA = 240;
float HEADLIGHT_SHADOW_WIDTH_BIKE = 2.75f;
float HEADLIGHT_SHADOW_WIDTH = 240;
float HEADLIGHT_SHADOW_WIDTH_SHORT = 8.0f;
float HEADLIGHT_SHADOW_WIDTH_LONG  = 10.0f;

// Coronas
float HEADLIGHT_CORONA_SIZE_SHORT = 0.075f;
float HEADLIGHT_CORONA_SIZE_LONG = 0.115f;

float HEADLIGHT_CORONA_ALPHA_SHORT = 128;
float HEADLIGHT_CORONA_ALPHA_LONG = 255;

//extern bool isRenderShadow = false;
int alpha = 150;
eLightState eIndicatorStatus = eLightState::IndicatorNone;

// Indicator lights
static uint64_t delay;
static bool delayState;

CAudioStream *m_LightOn, *m_LightOff, *signal_1, *signal_2, *beep;

void Musics()
{
    static bool loaded = false;
    
    CPed *player = FindPlayerPed();
    
    if(!player)
        return;
    
    if(!loaded)
    {
        m_LightOn = soundsys->LoadStream("sfx/avf/light_on.mp3");
        m_LightOff = soundsys->LoadStream("sfx/avf/light_off.mp3");
        signal_click = soundsys->LoadStream("sfx/avf/signal_click.mp3");
        signal_1 = soundsys->LoadStream("sfx/avf/signal_1.mp3");
        signal_2 = soundsys->LoadStream("sfx/avf/signal_2.mp3");
        beep = soundsys->LoadStream("sfx/avf/beep.mp3");
        loaded = true;
    }
}


processScriptsEvent = []
{
    Musics();
};

bool IsTruck(CVehicle* vehicle) {
    if (!vehicle) return false;

    int modelId = vehicle->m_nModelIndex;

    // Check against all truck model IDs
    switch (modelId) {
        case 403: // Linerunner
        case 514: // Tanker
        case 515: // Roadtrain
        case 578: // DFT-30
        case 443: // Packer
        case 455: // Flatbed
        case 456: // Yankee
        case 499: // Benson
        case 414: // Mule
        case 498: // Boxville
        case 408: // Trashmaster
        case 486: // Dumper
        case 433: // Barracks
        case 524: // Cement Truck
        case 525: // Towtruck
        case 552: // Utility Van
            return true;
    }
    return false;
}

bool IsBus(CVehicle* vehicle) {
    if (!vehicle) return false;

    int modelId = vehicle->m_nModelIndex;

    // Check against all bus model IDs
    switch (modelId) {
        case 431: // Bus
        case 437: // Coach
        //case 438: // Cabbie
        //case 444: // Monster Truck (assuming Monster A for unique purposes)
            return true;
    }
    return false;
}


void ProcessReverseSfx()
{
    static bool play = false;
    static bool loaded = false;
    CPed *player = FindPlayerPed();
    if(player && player->m_nPedFlags.bInVehicle)
    {
        CVehicle *vehicle = player->m_pMyVehicle;
        if(!vehicle)
            return;
        
        if(!IsTruck(vehicle))
            return;
            
        /*if(!loaded)
        {
            beep = soundsys->LoadStream("sfx/avf/beep.mp3");
            
            loaded = true;
        }*/
        
        if(beep)
        {
            beep->SetType(SoundEffect);
            beep->SetVolume(GetEffectsVolume());
            beep->SetLooping(true);
            
            int gear = vehicle->m_nCurrentGear;
            if(gear == 0)
            {
                if(!play)
                {
                    beep->Play();
                    play = true;
                }
            }
            else
            {
                beep->Stop();
                play = false;
            }
        }
    }
}

void Lights::InitSounds()
{
   /* m_LightOnStream = (C3DAudioStream *)soundsys->LoadStream("sfx/avf/light_on.mp3", true);
    m_LightOffStream = (C3DAudioStream *)soundsys->LoadStream("sfx/avf/light_off.mp3", true);*/
    Musics();
}

void Lights::Initialize() {
    
    //SetEventBeforeEx(processScripts, InitSounds);

    VehicleMaterials::Register([](CVehicle* vehicle, RpMaterial* material) {
        if (material->color.red == 255 && material->color.green == 173 && material->color.blue == 0)
            RegisterMaterial(vehicle, material, eLightState::Reverselight);

        else if (material->color.red == 0 && material->color.green == 255 && material->color.blue == 198)
            RegisterMaterial(vehicle, material, eLightState::Reverselight);

        else if (material->color.red == 184 && material->color.green == 255 && material->color.blue == 0)
            RegisterMaterial(vehicle, material, eLightState::Brakelight);

        else if (material->color.red == 255 && material->color.green == 59 && material->color.blue == 0)
            RegisterMaterial(vehicle, material, eLightState::Brakelight);

        else if (material->color.red == 0 && material->color.green == 16 && material->color.blue == 255)
            RegisterMaterial(vehicle, material, eLightState::Nightlight);

        else if (material->color.red == 0 && material->color.green == 17 && material->color.blue == 255)
            RegisterMaterial(vehicle, material, eLightState::AllDayLight);
        else if (material->color.red == 0 && material->color.green == 18 && material->color.blue == 255)
            RegisterMaterial(vehicle, material, eLightState::Daylight);

        else if (material->color.red == 255 && material->color.green == 174 && material->color.blue == 0)
            RegisterMaterial(vehicle, material, eLightState::FogLight);
        else if (material->color.red == 0 && material->color.green == 255 && material->color.blue == 199)
            RegisterMaterial(vehicle, material, eLightState::FogLight);
            
        else if (material->color.red == 255 && material->color.green == 175 && material->color.blue == 0)
            RegisterMaterial(vehicle, material, eLightState::FrontLightLeft);
        else if (material->color.red == 0 && material->color.green == 255 && material->color.blue == 200) 
            RegisterMaterial(vehicle, material, eLightState::FrontLightRight);
        else if (material->color.red == 255 && material->color.green == 60 && material->color.blue == 0) 
            RegisterMaterial(vehicle, material, eLightState::TailLightRight);
        else if (material->color.red == 185 && material->color.green == 255 && material->color.blue == 0)
            RegisterMaterial(vehicle, material, eLightState::TailLightLeft);

        // Indicator Lights
        eDummyPos pos = eDummyPos::None;
        if (material->color.blue == 0) {
            if (material->color.red == 255) { // Right
                if (material->color.green >= 56 && material->color.green <= 59) {
                    if (material->color.green == 58) {
                        pos = eDummyPos::FrontRight;
                    } else if (material->color.green == 57) {
                        pos = eDummyPos::MiddleRight;
                    } else if (material->color.green == 56) {
                        pos = eDummyPos::RearRight;
                    }
                    RegisterMaterial(vehicle, material, eLightState::IndicatorRight, pos);
                }
            }
            else if (material->color.green == 255) { // Left
                if (material->color.red >= 181 && material->color.red <= 184) {
                    if (material->color.red == 183) {
                        pos = eDummyPos::FrontLeft;
                    } else if (material->color.red == 182) {
                        pos = eDummyPos::MiddleLeft;
                    } else if (material->color.red == 181) {
                        pos = eDummyPos::RearLeft;
                    }
                    RegisterMaterial(vehicle, material, eLightState::IndicatorLeft, pos);
                }
            }
        }

        if (material->color.red == 255 
        && (material->color.green == 4 ||  material->color.green == 5) 
        && material->color.blue == 128 
        && std::string(material->texture->name).rfind("light", 0) == 0) {
            RegisterMaterial(vehicle, material, (material->color.green == 4) ? eLightState::IndicatorLeft : eLightState::IndicatorRight);
        }

        return material;
	});

    VehicleMaterials::RegisterDummy([](CVehicle* pVeh, RwFrame* frame, std::string name, bool parent) {
        eLightState state = eLightState::None;
        eDummyPos rotation = eDummyPos::Rear;
        RwRGBA col{ 255, 255, 255, 128 };

        std::smatch match;
        if (std::regex_search(name, match, std::regex("^fogl(ight)?_[lr].*$"))) {
            state = (toupper(match.str(2)[0]) == 'L') ? (eLightState::FogLight) : (eLightState::FogLight);
        } else if (std::regex_search(name, std::regex( "^rev.*\s*_[lr].*$"))) {
            state = eLightState::Reverselight;
            col = {240, 240, 240, 128};
        } else if (std::regex_search(name, std::regex("^light_day"))) {
            state = eLightState::Daylight;
        } else if (std::regex_search(name, std::regex("^light_night"))) {
            state = eLightState::Nightlight;
        } else if (std::regex_search(name, std::regex("^light_em"))) {
            state = eLightState::AllDayLight;
        } else if (std::regex_search(name, match, std::regex("^(turnl_|indicator_)(.{2})"))) { // Indicator Lights
            std::string stateStr = match.str(2);
            eLightState state = (toupper(stateStr[0]) == 'L') ? eLightState::IndicatorLeft : eLightState::IndicatorRight;
            eDummyPos rot = eDummyPos::None;
            
            if (toupper(stateStr[1]) == 'F') {
                rot = state == eLightState::IndicatorRight ? eDummyPos::FrontRight : eDummyPos::FrontLeft;
            } else if (toupper(stateStr[1]) == 'R') {
                rot = state == eLightState::IndicatorRight ? eDummyPos::RearRight : eDummyPos::RearLeft;
            } else if (toupper(stateStr[1]) == 'M') {
                rot = state == eLightState::IndicatorRight ? eDummyPos::MiddleRight : eDummyPos::MiddleLeft;
            }

            if (rot != eDummyPos::None) {
                bool exists = false;
                for (auto e: m_Dummies[pVeh][state]) {
                    if (e->Position.y == frame->modelling.pos.y
                    && e->Position.z == frame->modelling.pos.z) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    //LOG_VERBOSE("Registering {} for {}", name, pVeh->m_nModelIndex);
                    m_Dummies[pVeh][state].push_back(new VehicleDummy(frame, name, parent, rot, { 255, 128, 0, 128 }));
                }
            }
        }

        if (state != eLightState::None) {
            m_Dummies[pVeh][state].push_back(new VehicleDummy(frame, name, parent, rotation, col));
        }
	});
    
    processScriptsEvent = []() {
        size_t timestamp = CTimer::m_snTimeInMilliseconds;
        if ((timestamp - delay) > 500) {
            delay = timestamp;
            delayState = !delayState;
		}
        
        CVehicle *pVeh = FindPlayerVehicle(-1, false);
        if (pVeh) {
            static size_t prev = 0;
            if (Command<Commands::IS_WIDGET_SWIPED_LEFT>(WIDGET_HORN) && !m_Dummies[pVeh][eLightState::FogLight].empty()) {
                size_t now = CTimer::m_snTimeInMilliseconds;
                if (now - prev > 500.0f) {
                    LVehData& data = lVehData[pVeh];
                    data.m_bFogLightsOn = !data.m_bFogLightsOn;
                    prev = now;
                    
                    if(m_LightOn && m_LightOff)
                    {
                        CAudioStream *ptr = data.m_bFogLightsOn ? m_LightOn : m_LightOff;
                       // ptr->SetProgress(0.0f);
                        ptr->SetVolume(GetEffectsVolume());
					    ptr->Play();
                    }
                }
            }

            if (Command<Commands::IS_WIDGET_SWIPED_RIGHT>(WIDGET_HORN)) {
                size_t now = CTimer::m_snTimeInMilliseconds;
                if (now - prev > 500.0f) {
                    LVehData& data = lVehData[pVeh];
                    data.m_bLongLightsOn = !data.m_bLongLightsOn;
                    prev = now;
                    
                    if(m_LightOn && m_LightOff)
                    {
                        CAudioStream *ptr = data.m_bLongLightsOn ? m_LightOn : m_LightOff;
                       // ptr->SetProgress(0.0f);
                        ptr->SetVolume(GetEffectsVolume());
                        ptr->Play();
                    }
                }
            }
		}
    };

    SetEventBefore(processScripts);
    
    VehicleMaterials::RegisterRender([](CVehicle* pVeh) {
        int model = pVeh->m_nModelIndex;
        LVehData& data = lVehData[pVeh];

        if (pVeh->m_fHealth == 0) { // TODO
            return;
        }


        if (!m_Materials[pVeh->m_nModelIndex].empty()) {
            CAutomobile* automobile = reinterpret_cast<CAutomobile*>(pVeh);

            float vehicleAngle = (pVeh->GetHeading() * 180.0f) / 3.14f;
            float cameraAngle = (TheCamera.GetHeading() * 180.0f) / 3.14f;

            RenderLights(pVeh, eLightState::AllDayLight, vehicleAngle, cameraAngle);

            if (IsNightTime()) {
                RenderLights(pVeh, eLightState::Nightlight, vehicleAngle, cameraAngle);
            } else {
                RenderLights(pVeh, eLightState::Daylight, vehicleAngle, cameraAngle);
            }
            
            bool leftOk = !automobile->m_damageManager.GetLightStatus(eLights::LIGHT_FRONT_LEFT);
            bool rightOk = !automobile->m_damageManager.GetLightStatus(eLights::LIGHT_FRONT_RIGHT);
            if (data.m_bFogLightsOn) {
                CVector posn = reinterpret_cast<CVehicleModelInfo *>(CModelInfo::ms_modelInfoPtrs[pVeh->m_nModelIndex])->m_pStructure->m_positions[0];
                RenderLights(pVeh, eLightState::FogLight, vehicleAngle, cameraAngle, false, "foglight_single", 1.0f);
                if (leftOk && rightOk) {
                    posn.x = 0.0f;
                    posn.y += 4.2f;
                    Common::RegisterShadow(pVeh, posn, 225, 225, 225, GetShadowAlphaForDayTime(), 180.0f, 0.0f, "foglight_twin", 2.0f);
                } else {
                    posn.x = leftOk ? -0.5f : 0.5f;
                    posn.y += 3.2f;
                    Common::RegisterShadow(pVeh, posn, 225, 225, 225, GetShadowAlphaForDayTime(), 180.0f, 0.0f, "foglight_single", 1.2f);
                }
            }

            
            if (pVeh->m_nVehicleFlags.bLightsOn) {
                LVehData& data = lVehData[pVeh];
                if (leftOk && m_Materials[pVeh->m_nModelIndex][eLightState::FrontLightLeft].size() != 0) {
                    RenderLights(pVeh, eLightState::FrontLightLeft, vehicleAngle, cameraAngle);
                }

                if (rightOk && m_Materials[pVeh->m_nModelIndex][eLightState::FrontLightRight].size() != 0) {
                    RenderLights(pVeh, eLightState::FrontLightRight, vehicleAngle, cameraAngle);
                }
            }

            bool isBike = CModelInfo::IsBikeModel(pVeh->m_nModelIndex);
            if (isBike || CModelInfo::IsCarModel(pVeh->m_nModelIndex)) {

                CVehicle *pCurVeh = pVeh;

                if (pVeh->m_pTrailer) {
                    pCurVeh = pVeh->m_pTrailer;
                }

                if (pVeh->m_pTractor) {
                    pCurVeh = pVeh->m_pTractor;
                }

                if (pVeh->m_nRenderLightsFlags) {
                    RenderLights(pCurVeh, eLightState::TailLightLeft, vehicleAngle, cameraAngle);
                    RenderLights(pCurVeh, eLightState::TailLightRight, vehicleAngle, cameraAngle);
                }

                if (pVeh->m_fBrakePedal && pVeh->m_pDriver) {
                    RenderLights(pCurVeh, eLightState::Brakelight, vehicleAngle, cameraAngle);
                }

                bool isRevlightSupportedByModel = !m_Dummies[pCurVeh][eLightState::Reverselight].empty();

                bool globalRevlights = cfg->Bind("GlobalReverseLights", false, "Features");
                bool reverseLightsOn = !isBike && (isRevlightSupportedByModel || globalRevlights)
                    && pVeh->m_nCurrentGear == 0 && (pVeh->m_fMovingSpeed >= 0.01f) && pVeh->m_pDriver;
                
                if (reverseLightsOn) {
                    if (isRevlightSupportedByModel) {
                        RenderLights(pCurVeh, eLightState::Reverselight, vehicleAngle, cameraAngle);
                    } else if (globalRevlights) {
                        DrawGlobalLight(pVeh, eDummyPos::RearLeft, {240, 240, 240, 128});
                        DrawGlobalLight(pVeh, eDummyPos::RearRight, {240, 240, 240, 128});
                    }
                }

                if (pVeh->m_nRenderLightsFlags) {
                    CVector posn = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::ms_modelInfoPtrs[pCurVeh->m_nModelIndex])->m_pStructure->m_positions[1];
                    posn.x = 0.0f;
                    posn.y += 0.2f;
                    int r = 250;
                    int g = 0;
                    int b = 0;

                    if (pVeh->m_fBrakePedal) { // do twice
                        Common::RegisterShadow(pCurVeh, posn, r, g, b, GetShadowAlphaForDayTime(), 180.0f, 0.0f, isBike ? "taillight_bike" : "taillight", 1.75f);
                    }
                    Common::RegisterShadow(pCurVeh, posn, r, g, b, GetShadowAlphaForDayTime(), 180.0f, 0.0f, isBike ? "taillight_bike" : "taillight", 1.75f);
                }
            }
		}

        // Indicator Lights
        eLightState state = data.m_nIndicatorState;
        if(!cfg->Bind("Idicators", true, "Features")->GetBool())
            return;
        
        if (!cfg->Bind("GlobalIndicatorLights", false, "Features") && m_Dummies[pVeh].size() == 0 && m_Materials[pVeh->m_nModelIndex][state].size() == 0) {
            return;
        }

        /*if (CCutsceneMgr::ms_running || TheCamera.m_bWideScreenOn) {
            return;
        }*/
        
        static bool play = false;
        static bool play2 = false;
        
        if (!delayState) {
            if(signal_2 && state != eLightState::IndicatorNone)
            {
        // Light is off (dimmed)
        rColor = CRGBA(50, 50, 50, alpha);
        lColor = CRGBA(50, 50, 50, alpha);
        hColor = CRGBA(50, 50, 50, alpha);
        signal_2->SetVolume(GetEffectsVolume());
        signal_2->SetType(SoundEffect);
        if(!play){
           signal_2->Play();
           play = true;
           play2 = false;
        }
        }
        else{
            signal_2->Stop();
            play = false;
            play2 = false;
        }
    } else {
        // Light is on (brightened)
        if(signal_1 && state != eLightState::IndicatorNone)
            
            {
                signal_1->SetVolume(GetEffectsVolume());
                signal_1->SetType(SoundEffect);
                if(!play2)
                {
                                    signal_1->Play();
                                    play2 = false;
                                    play = true;
                }
            }
            else
            {
                signal_1->Stop();
                play = false;
            play2 = false;
            }
        if (state == eLightState::IndicatorLeft) {
            lColor = CRGBA(255, 255, 255, alpha); // Brighten left indicator
            rColor = CRGBA(50, 50, 50, alpha);    // Dim right indicator
            hColor = CRGBA(50, 50, 50, alpha);    // Dim hazard lights
        } else if (state == eLightState::IndicatorRight) {
            rColor = CRGBA(255, 255, 255, alpha); // Brighten right indicator
            lColor = CRGBA(50, 50, 50, alpha);    // Dim left indicator
            hColor = CRGBA(50, 50, 50, alpha);    // Dim hazard lights
        } else if (state == eLightState::IndicatorBoth) {
            hColor = CRGBA(255, 255, 255, alpha); // Brighten hazard lights
            rColor = CRGBA(50, 50, 50, alpha);    // Dim right indicator
            lColor = CRGBA(50, 50, 50, alpha);    // Dim left indicator
        }
    }
        
        if (pVeh->m_pDriver == FindPlayerPed()) {
            if(GetLightsStatus() == eLightState::None)
            {
               delay = 0;
               delayState = false;
            }
            data.m_nIndicatorState = GetLightsStatus();
        }

        if (pVeh->m_pTrailer) {
            LVehData &trailer = lVehData[pVeh->m_pTrailer];
            trailer.m_nIndicatorState = data.m_nIndicatorState;
            data.m_nIndicatorState = eLightState::IndicatorNone;
        }

        if (pVeh->m_pTractor) {
            LVehData &trailer = lVehData[pVeh->m_pTractor];
            trailer.m_nIndicatorState = data.m_nIndicatorState;
            data.m_nIndicatorState = eLightState::IndicatorNone;
        }

        if (!delayState || state == eLightState::IndicatorNone) {
            return;
        }

        // global turn lights
        if (cfg->Bind("GlobalIndicatorLights", false, "Features")->GetBool() &&
           (m_Dummies[pVeh][eLightState::IndicatorLeft].size() == 0 || m_Dummies[pVeh][eLightState::IndicatorRight].size() == 0)
             && m_Materials[pVeh->m_nModelIndex][state].size() == 0)
        {
            if ((pVeh->m_nVehicleSubClass == VEHICLE_AUTOMOBILE || pVeh->m_nVehicleSubClass == VEHICLE_BIKE) &&
                (pVeh->GetVehicleAppearance() == VEHICLE_APPEARANCE_AUTOMOBILE || pVeh->GetVehicleAppearance() == VEHICLE_APPEARANCE_BIKE) &&
                pVeh->m_nVehicleFlags.bEngineOn && pVeh->m_fHealth > 0 && !pVeh->m_nVehicleFlags.bIsDrowning && !pVeh->m_pAttachToEntity )
            {
                if (DistanceBetweenPoints(TheCamera.m_vecGameCamPos, pVeh->GetPosition()) < 150.0f) {
                    DrawVehicleTurnlights(pVeh, state);
                }
            }
        } else {
            int id = 0;
            if (state == eLightState::IndicatorBoth || state == eLightState::IndicatorLeft) {
                bool FrontDisabled = false;
                bool RearDisabled = false;
                bool MidDisabled = false;

                for (auto e: m_Dummies[pVeh][eLightState::IndicatorLeft]) {
                    if (e->PartType != eDetachPart::Unknown && IsBumperOrWingDamaged(pVeh, e->PartType)) {
                        if (e->Type == eDummyPos::FrontLeft) {
                            FrontDisabled = true;
                        }
                        if (e->Type == eDummyPos::MiddleLeft) {
                            MidDisabled = true;
                        }
                        if (e->Type == eDummyPos::RearLeft) {
                            RearDisabled = true;
                        }
                        continue;
                    }
                    EnableDummy((int)pVeh + id++, e, pVeh);
                    //Common::RegisterShadow(pVeh, e->ShdwPosition, e->Color.red, e->Color.green, e->Color.blue, GetShadowAlphaForDayTime(), e->Angle, e->CurrentAngle, "indicator");
                }

                for (auto e: m_Materials[pVeh->m_nModelIndex][eLightState::IndicatorLeft]){
                    if ((FrontDisabled && e->Pos == eDummyPos::FrontLeft)
                    || RearDisabled && e->Pos == eDummyPos::RearLeft
                    || MidDisabled && e->Pos == eDummyPos::MiddleLeft) {
                        continue;
                    }
                    EnableMaterial(e);
                }
            }

            if (state == eLightState::IndicatorBoth || state == eLightState::IndicatorRight) {
                bool FrontDisabled = false;
                bool RearDisabled = false;
                bool MidDisabled = false;

                for (auto e: m_Dummies[pVeh][eLightState::IndicatorRight]) {
                    if (e->PartType != eDetachPart::Unknown && IsBumperOrWingDamaged(pVeh, e->PartType)) {
                        if (e->Type == eDummyPos::FrontRight) {
                            FrontDisabled = true;
                        }
                        if (e->Type == eDummyPos::MiddleRight) {
                            MidDisabled = true;
                        }
                        if (e->Type == eDummyPos::RearRight) {
                            RearDisabled = true;
                        }
                        continue;
                    }
                    EnableDummy((int)pVeh + id++, e, pVeh);
                   // Common::RegisterShadow(pVeh, e->ShdwPosition, e->Color.red, e->Color.green, e->Color.blue, GetShadowAlphaForDayTime(), e->Angle, e->CurrentAngle, "indicator");
                }

                for (auto &e: m_Materials[pVeh->m_nModelIndex][eLightState::IndicatorRight]) {
                    if ((FrontDisabled && e->Pos == eDummyPos::FrontRight)
                    || RearDisabled && e->Pos == eDummyPos::RearRight
                    || MidDisabled && e->Pos == eDummyPos::MiddleRight) {
                        continue;
                    }
                    EnableMaterial(e);
                }
            }
		}
	});
};

void Lights::RenderLights(CVehicle* pVeh, eLightState state, float vehicleAngle, float cameraAngle, bool shadows, std::string texture, float sz) {
    bool flag = true;
    int id = 0;
    for (auto e: m_Dummies[pVeh][state]) {
        if (e->PartType != eDetachPart::Unknown && IsBumperOrWingDamaged(pVeh, e->PartType)) {
            flag = false;
            if (state == eLightState::FogLight) {
                lVehData[pVeh].m_bFogLightsOn = false;
            }
            continue;
        }
        EnableDummy((int)pVeh + (int)state + id++, e, pVeh);

        if (shadows) {
            Common::RegisterShadow(pVeh, e->ShdwPosition, e->Color.red, e->Color.green, e->Color.blue, GetShadowAlphaForDayTime(),  e->Angle, e->CurrentAngle, texture, sz);
        }
    }

    if (flag) {
        for (auto &e: m_Materials[pVeh->m_nModelIndex][state]) {
            EnableMaterial(e);
        }
    }
};

void Lights::RegisterMaterial(CVehicle* pVeh, RpMaterial* material, eLightState state, eDummyPos pos) {
    material->color.red = material->color.green = material->color.blue = 255;
    m_Materials[pVeh->m_nModelIndex][state].push_back(new VehicleMaterial(pVeh, material, pos));
};

void Lights::EnableDummy(int id, VehicleDummy* dummy, CVehicle* vehicle) {
    if (cfg->Bind("RenderCoronas", false, "Features")->GetBool()) {
        Common::RegisterCoronaWithAngle(vehicle, dummy->Position, dummy->Color.red, dummy->Color.green, dummy->Color.blue, 
            60, dummy->Angle, 0.3f,  0.3f);
    }
};

void Lights::EnableMaterial(VehicleMaterial* material) {
    if (material && material->Material) {
        VehicleMaterials::StoreMaterial(std::make_pair(reinterpret_cast<unsigned int*>(&material->Material->surfaceProps.ambient), *reinterpret_cast<unsigned int*>(&material->Material->surfaceProps.ambient)));
        material->Material->surfaceProps.ambient = 4.0;
        VehicleMaterials::StoreMaterial(std::make_pair(reinterpret_cast<unsigned int*>(&material->Material->texture), *reinterpret_cast<unsigned int*>(&material->Material->texture)));
        material->Material->texture = material->TextureActive;
    }
};

#include "funcs/rain.h"

static void FrameSetRotateYOnly(RwFrame *component, float angle) {
        CMatrix matrix(&component->modelling, false);
        matrix.SetRotateYOnly(angle);
        matrix.UpdateRW();
}

static void FrameSetRotateAndPositionZ(RwFrame *hub, RwFrame *wheel, float sign) {
        float angleZ = CGeneral::GetATanOfXY(sign * wheel->modelling.right.x, sign * wheel->modelling.right.y) - 3.141593f;
        CMatrix matrix(&hub->modelling, false);
        matrix.SetRotateZOnly(angleZ);
        matrix.UpdateRW();
        matrix.pos.z = wheel->modelling.pos.z;
        matrix.UpdateRW();
    }

#include "GTASA/NodeName.h"
#include "GTASA/CWeather.h"
#include "new_funcs/SimplexNoise.h"
//#include "new_funcs/list.h"
#include "Ehfuncs/meter.h"
#include "Ehfuncs/wheelhub.h"
#include "Ehfuncs/handlebar.h"
#include "Ehfuncs/chain.h"
//#include "Ehfuncs/brakes.h"
#include "vehfuncs/CustomSeed.h"
#include "vehfuncs/VehFuncs.h"

std::map<CVehicle*, VehData> vehicleDataMap;

/*ModelExtras/IvehFt*/

void Chain::Process(void* ptr, RwFrame* frame, eModelEntityType type) {
    CVehicle *pVeh = static_cast<CVehicle*>(ptr);
    CVehData &data = cVehData[pVeh];

    if (!data.m_bInitialized) {
        CVehData &data = cVehData[(CVehicle*)pVeh];
        Util::StoreChilds(frame, data.m_FrameList);
        data.m_bInitialized = true;
    }
    
    float speed = Util::GetVehicleSpeedRealistic(pVeh);
    if (pVeh->m_nVehicleSubClass == VEHICLE_BMX) {
        // Only move chain forward when pedal is rotating
        if (pVeh->m_fGasPedal && speed > 0) {
            if (data.m_nCurChain == 0)
                data.m_nCurChain = static_cast<short>(data.m_FrameList.size() - 1);
            else
                data.m_nCurChain -= 1;
        }
    } else {
        if (speed > 0.5) {
            if (data.m_nCurChain == 0)
                data.m_nCurChain = static_cast<short>(data.m_FrameList.size() - 1);
            else
                data.m_nCurChain -= 1;
        }

        if (speed < -0.5) {
            if (data.m_nCurChain == data.m_FrameList.size() - 1)
                data.m_nCurChain = 0;
            else
                data.m_nCurChain += 1;
        }
    }
    Util::HideAllChilds(frame);
    Util::ShowAllAtomics(data.m_FrameList[data.m_nCurChain]);
}

#define TARGET_NODE "handlebars"
#define SOURCE_NODE "forks_front"

void HandleBar::AddSource(void *ptr, RwFrame* frame, eModelEntityType type) {
    CVehicle *pVeh = static_cast<CVehicle*>(ptr);
    HVehData &data = hVehData[pVeh];
    data.m_pSource = frame;
}

void HandleBar::Process(void* ptr, RwFrame* frame, eModelEntityType type) {
    CVehicle *pVeh = static_cast<CVehicle*>(ptr);

    HVehData &data = hVehData[pVeh];
    if (data.m_pSource)  {
        float rot = Util::GetMatrixRotationZ(&data.m_pSource->modelling);
        Util::SetMatrixRotationZ(&frame->modelling, rot);
    }
}

void ProcessHubs(CVehicle *vehicle, std::list<RwFrame*> frames) {
    for (RwFrame* frame : frames) {
        if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle) {
            const std::string name = GetFrameNodeName(frame);

            // Rotate the hubs based on the wheel nodes
            CAutomobile* automobile = static_cast<CAutomobile*>(vehicle);
            if (name == "hub_lf" && automobile->m_aCarNodes[CAR_WHEEL_LF]) {
                FrameSetRotateAndPositionZ(frame, automobile->m_aCarNodes[CAR_WHEEL_LF], 1.0f);
            } else if (name == "hub_lb" && automobile->m_aCarNodes[CAR_WHEEL_LB]) {
                FrameSetRotateAndPositionZ(frame, automobile->m_aCarNodes[CAR_WHEEL_LB], 1.0f);
            } else if (name == "hub_rf" && automobile->m_aCarNodes[CAR_WHEEL_RF]) {
                FrameSetRotateAndPositionZ(frame, automobile->m_aCarNodes[CAR_WHEEL_RF], -1.0f);
            } else if (name == "hub_rb" && automobile->m_aCarNodes[CAR_WHEEL_RB]) {
                FrameSetRotateAndPositionZ(frame, automobile->m_aCarNodes[CAR_WHEEL_RB], -1.0f);
            }
        }
    }
}


void ProcessOdoMeter(CVehicle* vehicle, RwFrame* frame) {
    VehData &data = vehicleDataMap[vehicle];

    if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
    {
        std::string name = GetFrameNodeName(frame);
        if (!data.m_bOInitialized) {
            Util::StoreChilds(frame, data.m_OFrameList);
            data.m_nOTempVal = 1234 + rand() % (57842 - 1234);

            if (NODE_FOUND(name, "_kph")) {
                data.m_fOMul = 100;
            }
            data.m_bOInitialized = true;
        }

        // Calculate new value
        int rot = 0;
        if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE) {
            CBike *bike = (CBike*)vehicle;
            rot = static_cast<int>(bike->m_aWheelAngularVelocity[1]);
        } else {
            CAutomobile *am = (CAutomobile*)vehicle;
            rot = static_cast<int>(am->m_aWheelAngularVelocity[3]);
        }

        int rotVal = static_cast<int>((rot / (2.86* data.m_fOMul)));
        int val = std::stoi(data.m_OScreenText) + abs(data.m_nOTempVal - rotVal);
        data.m_nOTempVal = rotVal;

        if (val < 999999) {
            std::string showStr = std::to_string(val);

            // 1 -> 000001
            while (showStr.size() < 6) {
                showStr = "0" + showStr;
            }

            if (data.m_OScreenText != showStr) {
                // Update odometer value
                for (unsigned int i = 0; i < 6; i++) {
                    if (showStr[i] != data.m_OScreenText[i]) {
                        float angle = (std::stof(std::to_string(showStr[i])) - std::stof(std::to_string(data.m_OScreenText[i]))) * 36.0f;
                    Util::SetFrameRotationX(data.m_OFrameList[i], angle);
                    }
                }
                data.m_OScreenText = showStr;
            }
        }
    }
}

void ProcessRpmMeter(CVehicle* vehicle, RwFrame* frame) {

    if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
    {
        std::string name = GetFrameNodeName(frame);
        VehData &data = vehicleDataMap[vehicle];
        if (!data.m_bRInitialized) {
            data.m_nRMaxRpm = std::stoi(Util::GetRegexVal(name, ".*m([0-9]+).*", "100"));
            data.m_fRMaxRotation = std::stof(Util::GetRegexVal(name, ".*r([0-9]+).*", "100"));
            data.m_bRInitialized = true;
        }
    
        float rpm = 0.0f;
        float speed = Util::GetVehicleSpeedRealistic(vehicle);
        float delta = CTimer::ms_fTimeScale;

        if (vehicle->m_nCurrentGear != 0)
            rpm += 2.0f * delta * speed / vehicle->m_nCurrentGear;

        if (vehicle->m_nVehicleFlags.bEngineOn)
        rpm += 6.0f * delta;

        float new_rot = (data.m_fRMaxRotation / data.m_nRMaxRpm) * rpm * delta * 0.50f;
        new_rot = new_rot > data.m_fRMaxRotation ? data.m_fRMaxRotation : new_rot;
        new_rot = new_rot < 0 ? 0 : new_rot;

        float change = (new_rot - data.m_fRCurRotation) * 0.25f * delta;
        Util::SetFrameRotationY(frame, change);
        data.m_fRCurRotation += change;
    }
}

void ProcessSpeedMeter(CVehicle *vehicle, RwFrame* frame)
{
    VehData &data = vehicleDataMap[vehicle];
    if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
    {
        std::string name = GetFrameNodeName(frame);
        if (!data.m_bSInitialized) 
        {
            if (NODE_FOUND(name, "_kph")) {
            data.m_fSMul= 100.0f;
            }
            
            data.m_nSMaxSpeed = std::stoi(Util::GetRegexVal(name, ".*m([0-9]+).*", "50"));
            data.m_fSMaxRotation = std::stof(Util::GetRegexVal(name, ".*r([0-9]+).*", "100"));
            data.m_bSInitialized = true;
        }
        
        float speed = Util::GetVehicleSpeedRealistic(vehicle);
        float delta = CTimer::ms_fTimeScale;

        float totalRot = (data.m_fSMaxRotation / data.m_nSMaxSpeed) * speed * delta;
        totalRot = totalRot > data.m_fSMaxRotation ? data.m_fSMaxRotation : totalRot;
        totalRot = totalRot < 0 ? 0 : totalRot;

        float change = (totalRot - data.m_fSCurRotation) * 0.5f * delta;
        
        Util::SetFrameRotationY(frame, change);
        
        data.m_fSCurRotation += change;
    }
}

void ProcessTachoMeter(CVehicle *vehicle, RwFrame* frame)
{
    if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
    {
        std::string name = GetFrameNodeName(frame);
        VehData &data = vehicleDataMap[vehicle];

        // Initialize data if not already done
        if (!data.m_bInitialized) 
        {
            data.m_nMaxVal = std::stoi(Util::GetRegexVal(name, ".*m([0-9]+).*", "50"));
            data.m_fMaxRotation = std::stof(Util::GetRegexVal(name, ".*r([0-9]+).*", "100"));
            data.m_bInitialized = true;
        }

        // Get the current reading and compute the total rotation needed
        float reading = Util::GetVehicleSpeedRealistic(vehicle) / 5.0f;
        float delta = CTimer::ms_fTimeScale;

        // Compute the total rotation based on reading and max values
        float totalRot = (data.m_fMaxRotation / data.m_nMaxVal) * reading * delta;
        totalRot = std::clamp(totalRot, 0.0f, data.m_fMaxRotation);

        // Compute the change in rotation for this frame
        float change = (totalRot - data.m_fCurRotation) * 0.5f * delta;

        // Apply the rotation change
        Util::SetFrameRotationY(frame, change);

        // Update the current rotation
        data.m_fCurRotation = totalRot;
    }
}

/*VehFuncs*/

const float DEFAULT_SPEED = 0.0015f;

void ProcessAnims(CVehicle *vehicle, list<F_an*> items)
{
    VehData &data = vehicleDataMap[vehicle];
    for (F_an *an : items)
    {
        // we will not make previous-values-compatibility here
        // TODO: set this during the part store
        float speed = 0.0;
        float ax = 0;
        float ay = 0;
        float az = 0;
        float x = 0;
        float y = 0;
        float z = 0;

        int startNameIndex = 6;

        RwFrame *frame = an->frame;
        if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
        {
            bool open = false;
            int mode = an->mode;
            int submode = an->submode;
            int timeLimit = 0;
            bool validThisFrame = false;

            switch (mode)
            {
            case 0: // simple ping pong
                //if (useLog) lg << "Anims: Found 'f_an" << mode << "': ping pong \n";
                if (an->progress == 1.0f)
                {
                    if (an->opening)
                    {
                        open = false;
                        an->opening = false;
                    }
                    else
                    {
                        open = false;
                    }
                }
                else
                {
                    if (an->progress == 0.0f)
                    {
                        open = true;
                        an->opening = true;
                    }
                    else
                    {
                        open = an->opening;
                    }
                }
                break;
            case 1: // engine off
                switch (submode)
                {
                case 0:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": engine off \n";
                    if (!vehicle->m_nVehicleFlags.bEngineOn)
                        open = true;
                    else
                        open = false;
                    break;
                case 1:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": engine off or alarm on \n";
                    if (!vehicle->m_nVehicleFlags.bEngineOn)
                        open = true;
                    else
                        open = false;
                    break;
                }
                break;
            case 2: // occupant
                switch (submode)
                {
                case 0:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": driver \n";
                    if (vehicle->m_pDriver)
                        open = true;
                    else
                        open = false;
                    break;
                case 1:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": passenger 1 \n";
                    if (vehicle->m_apPassengers[0])
                        open = true;
                    else
                        open = false;
                    break;
                case 2:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": passenger 2 \n";
                    if (vehicle->m_apPassengers[1])
                        open = true;
                    else
                        open = false;
                    break;
                case 3:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": passenger 3 \n";
                    if (vehicle->m_apPassengers[2])
                        open = true;
                    else
                        open = false;
                    break;
                }
                break;
            case 3: // high speed
                switch (submode)
                {
                case 0:
                case 1:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed \n";
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed and f_spoiler \n";
                    if (((vehicle->m_vecMoveSpeed.Magnitude() * 50.0f) * 3.6f) > 100.0f)
                        open = true;
                    else
                        open = false;
                    break;
                }
                break;
            case 4: // brakes
                switch (submode)
                {
                case 0:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": brake \n";
                    if (vehicle->m_fBrakePedal > 0.5f)
                        open = true;
                    else
                        open = false;
                    break;
                case 1:
                case 2:
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed brake \n";
                    //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed brake and f_spoiler \n";
                    if (vehicle->m_fBrakePedal > 0.5f && ((vehicle->m_vecMoveSpeed.Magnitude() * 50.0f) * 3.6f) > 100.0f)
                        open = true;
                    else
                        open = false;
                    break;
                }
                break;
            // INTERNAL
            case 1001: //rain (f_wiper)
                startNameIndex = 7;
                timeLimit = 2000;
                ay = 60.0f;
                if (an->progress == 1.0f)
                {
                    if (an->opening)
                    {
                        open = false;
                        an->opening = false;
                    }
                    else
                    {
                        open = false;
                    }
                }
                else
                {
                    if (an->progress == 0.0f)
                    {
                        if (CWeather::Rain > 0.1f && vehicle->m_nVehicleFlags.bEngineOn)
                        {
                            if (CWeather::Rain > 0.4f || CTimer::m_snTimeInMilliseconds > an->nextTimeToOpen)
                            {
                                open = true;
                                an->opening = true;
                            }
                        }
                    }
                    else
                    {
                        open = an->opening;
                    }
                }
                break;
            }


            if (!mode == 0)
            {
                if (open)
                {
                    if (an->progress == 1.0f) continue;
                }
                else
                {
                    if (an->progress == 0.0f) continue;
                }
            }

            RestoreMatrixBackup(&frame->modelling, FRAME_EXTENSION(frame)->origMatrix);


            //f_an1a=ax45z10s2
            const string name = GetFrameNodeName(frame);


            // Get values (we always need it)
            if (name[startNameIndex] == '=')
            {
                speed = 0;
                ax = 0;
                ay = 0;
                az = 0;
                x = 0;
                y = 0;
                z = 0;

                for (unsigned int j = 6; j < name.length(); j++)
                {
                    switch (name[j])
                    {

                    case 'a':
                        switch (name[(j + 1)])
                        {
                        case 'x':
                            ax = stof(&name[(j + 2)]);
                            break;
                        case 'y':
                            ay = stof(&name[(j + 2)]);
                            break;
                        case 'z':
                            az = stof(&name[(j + 2)]);
                            break;
                        }
                        break;

                    case 's':
                        speed = stof(&name[(j + 1)]) * (CTimer::ms_fTimeStep * 1.66666f) * DEFAULT_SPEED;
                        break;

                    case 'x':
                        if (name[(j - 1)] == 'a') break;
                        x = stof(&name[(j + 1)]) * 0.01f;
                        break;

                    case 'y':
                        if (name[(j - 1)] == 'a') break;
                        y = stof(&name[(j + 1)]) * 0.01f;
                        break;

                    case 'z':
                        if (name[(j - 1)] == 'a') break;
                        z = stof(&name[(j + 1)]) * 0.01f;
                        break;
                    }

                    if (name[j] == '_') break;
                }
            }

            // Default speed value
            if (speed == 0.0f)
            {
                speed = 4 * (CTimer::ms_fTimeStep * 1.66666f) * DEFAULT_SPEED;
            }

            // Update progress and move it
            if (open)
            {
                an->progress += speed;
                if (an->progress > 1.0f) an->progress = 1.0f;
            }
            else
            {
                an->progress -= speed;
                if (an->progress < 0.0f)
                {
                    an->progress = 0.0f;
                    if (timeLimit > 0)
                    {
                        an->nextTimeToOpen = CTimer::m_snTimeInMilliseconds + timeLimit;
                    }
                }
            }
            
            RwV3d aX = {1.0f,0.0f,0.0f};
            RwV3d aY = {0.0f,1.0f,0.0f};
            RwV3d aZ = {0.0f,0.0f,1.0f};

            float progress = an->progress;
            if (progress != 0.0f)
            {
                if (x != 0 || y != 0 || z != 0)
                {
                    RwV3d vec = { (x * progress), (y * progress), (z * progress) };
                    RwFrameTranslate(frame, &vec, rwCOMBINEPRECONCAT);
                }
                if (ax != 0) RwFrameRotate(frame, &aX, (0.0f + progress * ax), rwCOMBINEPRECONCAT);
                if (ay != 0) RwFrameRotate(frame, &aY, (0.0f + progress * ay), rwCOMBINEPRECONCAT);
                if (az != 0) RwFrameRotate(frame, &aZ, (0.0f + progress * az), rwCOMBINEPRECONCAT);
            }
            RwFrameUpdateObjects(frame);
        }
        else
        {
            delete an;
            data.anims.remove(an);
        }
    }
}

void ProcessShake(CVehicle *vehicle, list<RwFrame*> frames)
{
    // Process dot
    VehData &data = vehicleDataMap[vehicle];
    data.dotLife += CTimer::ms_fTimeStep * (0.2f * ((data.smoothGasPedal * 2.0f) + 1.0f));
    if (data.dotLife >= 100.0f) data.dotLife = 1.0f;

    for (list<RwFrame*>::iterator it = frames.begin(); it != frames.end(); ++it)
    {
        RwFrame * frame = *it;
        if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
        {
            RestoreMatrixBackup(&frame->modelling, FRAME_EXTENSION(frame)->origMatrix);

            if (vehicle->m_nVehicleFlags.bEngineOn && vehicle->m_fHealth > 0 && !vehicle->m_nVehicleFlags.bEngineBroken && !vehicle->m_nVehicleFlags.bIsDrowning)
            {
                // Get noise
                float noise = SimplexNoise::noise(data.dotLife);

                const string name = GetFrameNodeName(frame);
                size_t found;

                // Mult noise
                found = name.find("_mu=");
                if (found != string::npos)
                {
                    float mult = stof(&name[found + 4]);
                    noise *= mult;
                }

                // Div noise by gas pedal 
                //noise /= ((xdata.smoothGasPedal * 1.0f) + 1.0f);

                // Convert noise to shake (angle)
                float angle = 0.0f;
                angle += noise * 0.7f;

                // Apply tilt
                found = name.find("_tl=");
                if (found != string::npos)
                {
                    float angMult = stof(&name[found + 6]);
                    angle += data.smoothGasPedal * angMult;
                }

                // Find axis
                RwV3d axis;
                RwV3d ax = {1.0f,0.0f,0.0f};
                RwV3d ay = {0.0f,1.0f,0.0f};
                RwV3d az = {0.0f,0.0f,1.0f};
                found = name.find("_x");
                if (found != string::npos)
                {
                    axis = ax;
                }
                else
                {
                    found = name.find("_z");
                    if (found != string::npos)
                    {
                        axis = az;
                    }
                    else axis = ay;
                }

                // Rotate
                RwFrameRotate(frame, &axis, angle, rwCOMBINEPRECONCAT);
            }
            RwFrameUpdateObjects(frame);
        }
        else
        {
            data.shakeFrame.remove(*it);
        }
    }
}

extern float iniDefaultSteerAngle = 100.0f;

void ProcessSteer(CVehicle *vehicle, list<RwFrame*> frames)
{
    VehData &data = vehicleDataMap[vehicle];
    for (RwFrame *frame : frames)
    {
        if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
        {
            const string name = GetFrameNodeName(frame);

            float angle = (vehicle->m_fSteerAngle * (-1.666666f));

            float maxAngle = iniDefaultSteerAngle;

            if (name[0] == 'f') {
                //f_steer180
                if (isdigit(name[7])) {
                    maxAngle = (float)stoi(&name[7]);
                }
                angle *= maxAngle;
            }
            else { //movsteer or steering
                //movsteer_0.5
                maxAngle = 1.0f;
                if (name[8] == '_') {
                    if (isdigit(name[9])) {
                        maxAngle = stof(&name[9]);
                    }
                }
                angle *= 90.0f;
                angle *= maxAngle;
            }
            RwV3d aY = {0.0f, 1.0f, 0.0f};
            RestoreMatrixBackup(&frame->modelling, FRAME_EXTENSION(frame)->origMatrix);
            FrameSetRotateYOnly(frame, (vehicle->m_fSteerAngle * (-7.0f)));
            /*RwFrameRotate(frame, &aY, angle, rwCOMBINEPRECONCAT);
            RwFrameUpdateObjects(frame);*/
        }
        else
        {
            data.steer.remove(frame);
        }
    }
}

void ProcessPedal(CVehicle *vehicle, list<RwFrame*> frames, int mode)
{
    VehData &data = vehicleDataMap[vehicle];
    for (RwFrame *frame : frames)
    {
        if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
        {
            RestoreMatrixBackup(&frame->modelling, FRAME_EXTENSION(frame)->origMatrix);

            if (vehicle->m_nVehicleFlags.bEngineOn && vehicle->m_fHealth > 0 && !vehicle->m_nVehicleFlags.bEngineBroken && !vehicle->m_nVehicleFlags.bIsDrowning)
            {

                const string name = GetFrameNodeName(frame);
                size_t found;

                float angle = 0.0;
                float pedal;

                if (mode == 1)
                {
                    pedal = data.smoothGasPedal;
                }
                else 
                {
                    pedal = data.smoothBrakePedal;
                } 

                // Find axis
                found = name.find("_ax=");
                if (found != string::npos)
                {
                    float angleX = stof(&name[found + 4]);
                    angle = 0.0f + pedal * angleX;
                    // Rotate
                    RwV3d axis = {1.0f,0.0f,0.0f};
                    RwFrameRotate(frame, &axis, angle, rwCOMBINEPRECONCAT);
                }

                found = name.find("_ay=");
                if (found != string::npos)
                {
                    float angleZ = stof(&name[found + 4]);
                    angle = 0.0f + pedal * angleZ;
                    // Rotate
                    RwV3d axis = {0.0f,1.0f,0.0f};
                    RwFrameRotate(frame, &axis, angle, rwCOMBINEPRECONCAT);
                }

                found = name.find("_az=");
                if (found != string::npos)
                {
                    float angleZ = stof(&name[found + 4]);
                    angle = 0.0f + pedal * angleZ;
                    // Rotate
                    RwV3d axis = {0.0f,0.0f,1.0f};
                    RwFrameRotate(frame, &axis, angle, rwCOMBINEPRECONCAT);
                }
            }
            RwFrameUpdateObjects(frame);
        }
        else
        {
            if (mode == 1) data.gaspedalFrame.remove(frame); else data.brakepedalFrame.remove(frame);
        }
    }
}

void ProcessRotatePart(CVehicle *vehicle, list<RwFrame*> frames, bool isGear)
{
    VehData &data = vehicleDataMap[vehicle];
    for (RwFrame *frame : frames)
    {
        if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
        {
            const string name = GetFrameNodeName(frame);

            float speedMult = 0.0f + CTimer::ms_fTimeStep * 22.0f;

            size_t found;
            found = name.find("_mu=");
            if (found != string::npos)
            {
                float mult = stof(&name[found + 4]);
                speedMult *= mult;
            }

            if (isGear == true) 
            {
                speedMult += CTimer::ms_fTimeStep * abs(data.smoothGasPedal * 13.0f);
            }

            RwV3d axis = {0.0f,1.0f,0.0f}; //y
            RwV3d ax = {1.0f,0.0f,0.0f};
            RwV3d az = {0.0f,0.0f,1.0f};

            // Find axis
            found = name.find("_x");
            if (found != string::npos)
            {
                axis = ax;
            }

            //found = name.find("_y");
            //if (found != string::npos)
            //{
            //  axis = (RwV3d *)0x008D2E0C;
            //}

            found = name.find("_z");
            if (found != string::npos)
            {
                axis = az;
            }

            RwFrameRotate(frame, &axis, speedMult, rwCOMBINEPRECONCAT);

            RwFrameUpdateObjects(frame);
        }
        else
        {
            if (isGear) data.gearFrame.remove(frame);
            else data.fanFrame.remove(frame);
        }
    }
}

const float DEFAULT_SPEED2 = 0.1f;

void ProcessFootpegs(CVehicle *vehicle, list<F_footpegs*> items, int mode)
{
    float speed = DEFAULT_SPEED2;  // Initialize with DEFAULT_SPEED2
    float ax = 90;
    float ay = 0;
    float az = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    VehData &data = vehicleDataMap[vehicle];

    for (auto footpegs : items)
    {
        RwFrame *frame = footpegs->frame;
        if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
        {
            bool open = false;

            if (mode == 1) 
            {
                open = (vehicle->m_pDriver != nullptr);
            }
            else 
            {
                open = (vehicle->m_apPassengers[0] != nullptr);
            }

            if ((open && footpegs->progress == 1.0f) || (!open && footpegs->progress == 0.0f)) 
            {
                continue;
            }

            RestoreMatrixBackup(&frame->modelling, FRAME_EXTENSION(frame)->origMatrix);

            //f_fpeg1=ax45z10s2
            const string name = GetFrameNodeName(frame);
            size_t found;
            
            RwV3d aX = {1.0f, 0.0f, 0.0f}; // y
            RwV3d aY = {0.0f, 1.0f, 0.0f};
            RwV3d aZ = {0.0f, 0.0f, 1.0f};

            // Get values (we always need it)
            if (name[7] == '=')
            {
                speed = DEFAULT_SPEED2;  // Default speed
                ax = ay = az = x = y = z = 0;

                for (unsigned int j = 7; j < name.length(); j++)
                {
                    switch (name[j])
                    {
                    case 'a':
                        switch (name[j + 1])
                        {
                        case 'x': ax = stof(&name[j + 2]); break;
                        case 'y': ay = stof(&name[j + 2]); break;
                        case 'z': az = stof(&name[j + 2]); break;
                        }
                        break;

                    case 's':
                        speed = stof(&name[j + 1]) * DEFAULT_SPEED2;
                        break;

                    case 'x':
                        if (name[j - 1] != 'a') x = stof(&name[j + 1]) * 0.01f;
                        break;

                    case 'y':
                        if (name[j - 1] != 'a') y = stof(&name[j + 1]) * 0.01f;
                        break;

                    case 'z':
                        if (name[j - 1] != 'a') z = stof(&name[j + 1]) * 0.01f;
                        break;
                    }

                    if (name[j] == '_') break;
                }
            }
            else 
            {
                // Retrocompatibility
                if (open)
                {
                    if (footpegs->progress == 1.0f) continue;
                    footpegs->progress += 0.2f * DEFAULT_SPEED2;
                    if (footpegs->progress > 1.0f) footpegs->progress = 1.0f;
                }
                else
                {
                    if (footpegs->progress == 0.0f) continue;
                    footpegs->progress -= 0.2f * DEFAULT_SPEED2;
                    if (footpegs->progress < 0.0f) footpegs->progress = 0.0f;
                }

                float angle;
                found = name.find("_ax=");
                if (found != string::npos)
                {
                    angle = stof(&name[found + 4]) * footpegs->progress;
                    RwFrameRotate(frame, &aX, angle, rwCOMBINEPRECONCAT);
                }
                found = name.find("_ay=");
                if (found != string::npos)
                {
                    angle = stof(&name[found + 4]) * footpegs->progress;
                    RwFrameRotate(frame, &aY, angle, rwCOMBINEPRECONCAT);
                }
                found = name.find("_az=");
                if (found != string::npos)
                {
                    angle = stof(&name[found + 4]) * footpegs->progress;
                    RwFrameRotate(frame, &aZ, angle, rwCOMBINEPRECONCAT);
                }
                RwFrameUpdateObjects(frame);
                continue;
            }

            // Update progress and move it
            if (open)
            {
                footpegs->progress += speed;
                if (footpegs->progress > 1.0f) footpegs->progress = 1.0f;
            }
            else
            {
                footpegs->progress -= speed;
                if (footpegs->progress < 0.0f) footpegs->progress = 0.0f;
            }

            float progress = footpegs->progress;
            if (progress != 0.0f)
            {
                if (x != 0 || y != 0 || z != 0)
                {
                    RwV3d vec = { x * progress, y * progress, z * progress };
                    RwFrameTranslate(frame, &vec, rwCOMBINEPRECONCAT);
                }
                if (ax != 0) RwFrameRotate(frame, &aX, progress * ax, rwCOMBINEPRECONCAT);
                if (ay != 0) RwFrameRotate(frame, &aY, progress * ay, rwCOMBINEPRECONCAT);
                if (az != 0) RwFrameRotate(frame, &aZ, progress * az, rwCOMBINEPRECONCAT);
            }
            RwFrameUpdateObjects(frame);
        }
        else
        {
            delete footpegs;
            if (mode == 1) data.fpegFront.remove(footpegs); else data.fpegBack.remove(footpegs);
        }
    }
}


void ProcessTrifork(CVehicle *vehicle, RwFrame* frame)
{
    VehData &data = vehicleDataMap[vehicle];
    if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
    {
        RwFrame *wheelFrame = CClumpModelInfo::GetFrameFromId(vehicle->m_pRwClump, 5);
        RwFrame *wheelFinalFrame = frame->child->child;
        if (wheelFrame && wheelFinalFrame)
        {
            /*
            frame->modelling.at.x = wheelFrame->modelling.at.x;
            frame->modelling.at.y = wheelFrame->modelling.at.y;
            frame->modelling.right.x = wheelFrame->modelling.right.x;
            frame->modelling.right.y = wheelFrame->modelling.right.y;
            frame->modelling.up.x = wheelFrame->modelling.up.x;
            frame->modelling.up.y = wheelFrame->modelling.up.y;
            */
            
            RwV3d ax = {1.0f,0.0f,0.0f};
            RwV3d az = {0.0f,0.0f,1.0f};

            float angle = Util::GetATanOfXY(wheelFrame->modelling.right.x, wheelFrame->modelling.right.y) * 57.295776f - 180.0f;
            while (angle < 0.0) angle += 360.0;

            RestoreMatrixBackup(&frame->modelling, FRAME_EXTENSION(frame)->origMatrix);
            Util::SetMatrixRotationZ(&frame->modelling, angle);
           // RwFrameRotate(frame, &az, angle, rwCOMBINEPRECONCAT);

            CAutomobile *autom = (CAutomobile *)vehicle;

            //RwV3d vec = RwV3d{ 0.0f, 0.0f, diffGuardWheel };
            //RwFrameTranslate(wheelFinalFrame, &vec, rwCOMBINEREPLACE);

            //RwFrameRotate(wheelFinalFrame, &ax, (autom->m_aWheelAngularVelocity[0] * 57.295776f), rwCOMBINEREPLACE);
            Util::SetMatrixRotationZ(&wheelFinalFrame->modelling, (autom->m_aWheelAngularVelocity[0] * 57.295776f));

            /*RwFrameUpdateObjects(wheelFinalFrame);
            RwFrameUpdateObjects(frame);*/
        }
    }
    else
    {
        data.triforkFrame = nullptr;
    }
}

CVehicle *veh;
CEntity *entity;
bool noLights;

void HookPopUp(GlossRegisters* regs, PHookHandle hook)
{
    //R1, [R4, #0x5A0]
    regs->regs.r1 = *(uint32_t*)(regs->regs.r4 + 0x5A0);
    
    veh = (CVehicle*)regs->regs.r4;
    entity = (CEntity*)veh;
    VehData &xdata = vehicleDataMap[veh];
    
    if (xdata.popupFrame[0] != nullptr)
    {
        noLights = false;
        CAutomobile *automobile = (CAutomobile*)veh;

        if (automobile->m_damageManager.GetLightStatus(LIGHT_FRONT_LEFT))
        {
            if (xdata.popupProgress[LIGHT_FRONT_RIGHT] != 1.0f)
            {
                noLights = true;
            }
        }
        else if (automobile->m_damageManager.GetLightStatus(LIGHT_FRONT_RIGHT))
        {
           if (xdata.popupProgress[LIGHT_FRONT_LEFT] != 1.0f)
           {
               noLights = true;
           }
        }
        else
        {
            if (xdata.popupProgress[LIGHT_FRONT_LEFT] != 1.0f && xdata.popupProgress[LIGHT_FRONT_RIGHT] != 1.0f)
            {
               noLights = true;
            }
        }
        //if (noLights) *(uint32_t*)(regs->regs.r0) = libs.pGame + 0x5916FA; //don't process lights
     }
}

void HookPoplights(GlossRegisters* regs, PHookHandle hook)
{
    VehData &xdata = vehicleDataMap[veh];
    
    if (xdata.popupFrame[0] != nullptr)
    {
       if (noLights) *(uint32_t*)(regs->regs.r0) = libs.pGame + 0x5916FA; //don't process lights
    }
}

void ProcessPopup(CVehicle *vehicle)
{
    float speed = 0.0f;
    float ax = 30;
    float ay = 0;
    float az = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    VehData &data = vehicleDataMap[vehicle];
    for (int i = 0; i < 2; i++)
    {
        if (data.popupFrame[i] != nullptr)
        {
            RwFrame *frame = data.popupFrame[i];
            if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
            {
                //f_popr=ax45z10s2
                const string name = GetFrameNodeName(frame);
                
                RwV3d aX = {1.0f,0.0f,0.0f}; //y
                RwV3d aY = {0.0f,1.0f,0.0f};
                RwV3d aZ = {0.0f,0.0f,1.0f};
                
                // Get values (we always need it)
                if (name[6] == '=')
                {
                    speed = 0;
                    ax = 0;
                    ay = 0;
                    az = 0;
                    x = 0;
                    y = 0;
                    z = 0;
                    for (unsigned int j = 7; j < name.length(); j++)
                    {
                        switch (name[j])
                        {

                        case 'a':
                            switch (name[(j + 1)])
                            {
                            case 'x':
                                ax = stof(&name[(j + 2)]);
                                break;
                            case 'y':
                                ay = stof(&name[(j + 2)]);
                                break;
                            case 'z':
                                az = stof(&name[(j + 2)]);
                                break;
                            }
                            break;

                        case 's':
                            speed = stof(&name[(j + 1)]) * (CTimer::ms_fTimeStep * 1.66666f) * 0.005f;
                            break;

                        case 'x':
                            if (name[(j - 1)] == 'a') break;
                            x = stof(&name[(j + 1)]) * 0.01f;
                            break;

                        case 'y':
                            if (name[(j - 1)] == 'a') break;
                            y = stof(&name[(j + 1)]) * 0.01f;
                            break;

                        case 'z':
                            if (name[(j - 1)] == 'a') break;
                            z = stof(&name[(j + 1)]) * 0.01f;
                            break;
                        }

                        if (name[j] == '_') break;
                    }
                }

                if (speed == 0.0f)
                { // default speed value
                    speed = 4 * (CTimer::ms_fTimeStep * 1.66666f) * 0.005f;
                }

                // Process if valid
                bool lightsOn;
                if (vehicle->m_nVehicleFlags.bLightsOn || vehicle->m_nOverrideLights == 2)
                {
                    if (data.popupProgress[i] == 1.0f) continue;
                    lightsOn = true;
                }
                else
                {
                    if (data.popupProgress[i] == 0.0f) continue;
                    lightsOn = false;
                }

                CAutomobile *automobile = (CAutomobile*)vehicle;
                if (i == LIGHT_FRONT_RIGHT) if (automobile->m_damageManager.GetLightStatus(LIGHT_FRONT_RIGHT)) continue;
                if (i == LIGHT_FRONT_LEFT) if (automobile->m_damageManager.GetLightStatus(LIGHT_FRONT_LEFT)) continue;


                // Up or down
                if (lightsOn)
                    data.popupProgress[i] += speed;
                else
                    data.popupProgress[i] -= speed;
                

                // Update progress and move it
                RestoreMatrixBackup(&frame->modelling, FRAME_EXTENSION(frame)->origMatrix);
                float progress = data.popupProgress[i];
                //if (useLog) lg << "popup: " << xdata->popupProgress[i] << " ax " << ax << " ay " << ay << " az " << az << " x " << x << " y " << y << " z " << z << " s " << speed << " \n";
                if (progress >= 1.0f)
                {
                    data.popupProgress[i] = 1.0f;
                    RwV3d vec = { x, y, z };
                    RwFrameTranslate(frame, &vec, rwCOMBINEPRECONCAT);
                    RwFrameRotate(frame, &aX, ax, rwCOMBINEPRECONCAT);
                    RwFrameRotate(frame, &aY, ay, rwCOMBINEPRECONCAT);
                    RwFrameRotate(frame, &aZ, az, rwCOMBINEPRECONCAT);
                }
                else
                {
                    if (progress <= 0.0f)
                    {
                        data.popupProgress[i] = 0.0f;
                        RwV3d vec = { 0, 0, 0 };
                        RwFrameTranslate(frame, &vec, rwCOMBINEPRECONCAT);
                        RwFrameRotate(frame, &aX, 0, rwCOMBINEPRECONCAT);
                        RwFrameRotate(frame, &aY, 0, rwCOMBINEPRECONCAT);
                        RwFrameRotate(frame, &aZ, 0, rwCOMBINEPRECONCAT);
                    }
                    else
                    {
                        RwV3d vec = { (x * progress), (y * progress), (z * progress) };
                        RwFrameTranslate(frame, &vec, rwCOMBINEPRECONCAT);
                        RwFrameRotate(frame, &aX, (0.0f + progress * ax), rwCOMBINEPRECONCAT);
                        RwFrameRotate(frame, &aY, (0.0f + progress * ay), rwCOMBINEPRECONCAT);
                        RwFrameRotate(frame, &aZ, (0.0f + progress * az), rwCOMBINEPRECONCAT);
                    }
                }
                RwFrameUpdateObjects(frame);
            }
            else
            {
                data.popupFrame[i] = nullptr;
                data.popupProgress[i] = 0.0f;
            }
        }
    }
}

bool IVFinstalled = false;
bool APPinstalled = false;
std::list<std::pair<unsigned int *, unsigned int>> resetMats;

CVehicle *curVehicle;

enum MatFuncType {
    nothing,
    ivf,
    taxi,
    brakeDisc
};
MatFuncType CheckMaterials(RpMaterial * material, RpAtomic * atomic);

MatFuncType CheckMaterials(RpMaterial * material, RpAtomic *atomic)
{
    if (material->color.red != 255 || material->color.green != 255 || material->color.blue != 255)
    {
        // Fix Improved Vehicle Features material colors
        // We are not fixing emergency lights here, at least for now, because need more conditions and that case is not important (like, the color is red)
        if (!IVFinstalled)
        {
            if (material->color.red == 255)
            {
                if (material->color.blue == 0)
                {
                    if (material->color.green >= 173 && material->color.green <= 175) return MatFuncType::ivf;
                    if (material->color.green >= 56 && material->color.green <= 60) return MatFuncType::ivf;
                }
            }
            else if (material->color.green == 255)
            {
                if (material->color.blue == 0)
                {
                    if (material->color.red >= 181 && material->color.red <= 185) return MatFuncType::ivf;
                }
                if (material->color.red == 0)
                {
                    if (material->color.blue >= 198 && material->color.blue <= 200) return MatFuncType::ivf;
                }
            }
            else if (material->color.blue == 255)
            {
                if (material->color.red == 0)
                {
                    if (material->color.green >= 16 && material->color.green <= 18) return MatFuncType::ivf;
                }
            }
        }
        if (material->color.blue == 255 && material->color.green == 255)
        {
            if (material->color.red == 1)
            {
                return MatFuncType::brakeDisc;
            }
        }
        atomic->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
    }

    if (material->texture)
    {
        string texName = material->texture->name;

        if (texName.length() == 8 && (texName[0] == '9' || texName[0] == 't'))
        {
            // Taxi sign
            size_t found = texName.find("92sign64");
            size_t found2 = texName.find("taxisign");
            if (found != string::npos || found2 != string::npos)
            {
                return MatFuncType::taxi;
            }
        }
    }

    return MatFuncType::nothing;
}

RpMaterial *MaterialCallback(RpMaterial *material, void *data)
{
    VehData &xdata = vehicleDataMap[curVehicle];

    switch (CheckMaterials(material, (RpAtomic *)data))
    {
    case MatFuncType::ivf:
        /*if (material->texture)
        {
            string name = material->texture->name;
            if (name[15] == '8')
            {
                if (name.length() == 16)
                {
                    size_t found = name.find("vehiclelights128");
                    if (found != string::npos)
                    {
                        if (useLog) lg << "Not valid IVF material \n";
                        break;
                    }
                }
            }
            else
            {
                if (name[15] == 'n')
                {
                    if (name.length() == 18)
                    {
                        size_t found = name.find("vehiclelightson128");
                        if (found != string::npos)
                        {
                            if (useLog) lg << "Not valid on IVF material \n";
                            break;
                        }
                    }
                }
            }
            break;
        }*/
        //if (useLog) lg << "Found IVF material \n";
        material->color.red = 0xFF; material->color.green = 0xFF; material->color.blue = 0xFF;
        break;
    case MatFuncType::taxi:
        xdata.taxiSignMaterial = material;
        break;
    case MatFuncType::brakeDisc:
        material->color.red = 0xFF; material->color.green = 0xFF; material->color.blue = 0xFF;
        xdata.brakeDiscMaterial = material;
        break;
    default:
        break;
    }

    return material;
}

RpAtomic *AtomicCallback(RpAtomic *atomic, void *data) 
{
    if (atomic->geometry) 
    {
        RpGeometryForAllMaterials(atomic->geometry, MaterialCallback, atomic);
    }
    return atomic;
}

void FixMaterials(RpClump * clump) 
{
    uint32_t data = 0;
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(clump), AtomicCallback, (void *)data);
    return;
}

void CloneNode(RwFrame *frame, RpClump * clump, RwFrame *parent, bool isRoot, bool isVarWheel)
{
    RwFrame * newFrame;

    if (isVarWheel) FRAME_EXTENSION(frame)->flags.bIsVarWheel = true;

    if (isRoot)
    {
        *(uint32_t*)libs.GetSym("_ZLl4gpClumpToAddTo") = (uint32_t)clump; //gpClumptoaddto
        RwFrameForAllObjects(frame, CopyObjectsCB, parent);
        if (RwFrame * nextFrame = frame->child) CloneNode(nextFrame, clump, parent, false, isVarWheel);
    }
    else
    {
        newFrame = RwFrameCreate();

        //CVisibilityPlugins::SetFrameHierarchyId(newFrame, 101);

        memcpy(&newFrame->modelling, &frame->modelling, sizeof(RwMatrix));
        RwMatrixUpdate(&newFrame->modelling);

        const string frameName = GetFrameNodeName(frame);
        SetFrameNodeName(newFrame, &frameName[0]);

        RpAtomic * atomic = (RpAtomic*)GetFirstObject(frame);

        if (atomic)
        {
            RpAtomic * newAtomic = RpAtomicClone(atomic);
            RpAtomicSetFrame(newAtomic, newFrame);
            RpClumpAddAtomic(clump, newAtomic);
            //CVisibilityPlugins::SetAtomicRenderCallback(newAtomic, (RpAtomic *(*)(RpAtomic *))0x007323C0);
        }

        RwFrameAddChild(parent, newFrame);

        if (RwFrame * nextFrame = frame->child) CloneNode(nextFrame, clump, newFrame, false, isVarWheel);
        if (RwFrame * nextFrame = frame->next)  CloneNode(nextFrame, clump, parent, false, isVarWheel);
    }
    return;
}

void DestroyNodeHierarchyRecursive(RwFrame * frame)
{
    RpAtomic * atomic = (RpAtomic *)GetFirstObject(frame);
    if (atomic != nullptr) 
    {
        RpClump * clump = atomic->clump;
        RpClumpRemoveAtomic(clump, atomic);
        RpAtomicDestroy(atomic);
    }
    
    RwFrameDestroy(frame);

    /////////////////////////////////////////
    if (RwFrame * newFrame = frame->child) DestroyNodeHierarchyRecursive(newFrame);
    if (RwFrame * newFrame = frame->next)  DestroyNodeHierarchyRecursive(newFrame);

    return;
}

void HideAllAtomics(RwFrame * frame)
{
    if (!rwLinkListEmpty(&frame->objectList))
    {
        RwObjectHasFrame * atomic;

        RwLLLink * current = rwLinkListGetFirstLLLink(&frame->objectList);
        RwLLLink * end = rwLinkListGetTerminator(&frame->objectList);

        current = rwLinkListGetFirstLLLink(&frame->objectList);
        while (current != end) {
            atomic = rwLLLinkGetData(current, RwObjectHasFrame, lFrame);
            atomic->object.flags &= ~0x4; // clear

            current = rwLLLinkGetNext(current);
        }
    }
    return;
}

void HideAllAtomicsExcept(RwFrame * frame, int except)
{
    if (!rwLinkListEmpty(&frame->objectList))
    {
        RwObjectHasFrame * atomic;

        RwLLLink * current = rwLinkListGetFirstLLLink(&frame->objectList);
        RwLLLink * end = rwLinkListGetTerminator(&frame->objectList);

        current = rwLinkListGetFirstLLLink(&frame->objectList);
        int i = 0;
        while (current != end)
        {
            atomic = rwLLLinkGetData(current, RwObjectHasFrame, lFrame);
            if (i == except) 
            {
                atomic->object.flags |= 0x4; // set
            }
            else 
            {
                atomic->object.flags &= ~0x4; // clear
            }

            current = rwLLLinkGetNext(current);
            i++;
        }
    }
    return;
}

RwFrame * HideAllNodesRecursive_Forced(RwFrame *frame, bool isRoot)
{
    //HideAllAtomics(frame);
    FRAME_EXTENSION(frame)->flags.bNeverRender = true;

    if (RwFrame * nextFrame = frame->child) HideAllNodesRecursive_Forced(nextFrame, false);
    if (!isRoot) if (RwFrame * nextFrame = frame->next)  HideAllNodesRecursive_Forced(nextFrame, false);
    return frame;
}

RwFrame * ShowAllNodesRecursive_Forced(RwFrame *frame, bool isRoot)
{
    //ShowAllAtomics(frame);
    FRAME_EXTENSION(frame)->flags.bNeverRender = false;

    if (RwFrame * nextFrame = frame->child) ShowAllNodesRecursive_Forced(nextFrame, false);
    if (!isRoot) if (RwFrame * nextFrame = frame->next)  ShowAllNodesRecursive_Forced(nextFrame, false);
    return frame;
}

int GetDefaultLodForInteriorMinor(CVehicle *vehicle) {
    int setLod = -2;
    if (vehicle->m_nVehicleFlags.bIsBig || vehicle->m_nVehicleFlags.bIsBus || vehicle->m_nVehicleSubClass == VEHICLE_PLANE || vehicle->m_nVehicleSubClass == VEHICLE_HELI)
    {
        setLod = -9;
    }
    return setLod;
}

///////////////////////////////////////////// Setup Speedo
// TODO: better code, like odometer

void SetupDigitalSpeedo(CVehicle * vehicle, RwFrame * frame) 
{
    VehData &data = vehicleDataMap[vehicle];
    int setLod = GetDefaultLodForInteriorMinor(vehicle);
    
    frame = frame->child;
    if (frame != nullptr) 
    {
        int i = 0;
        RwFrame * frameDigits = nullptr;
        RwFrame * frameDigitsRoot = nullptr;

        while (i < 4) 
        {
            const char * name = GetFrameNodeName(frame);

            // Find 'digits'
            if (frameDigits == nullptr) 
            {
                if (strstr(name, "digits"))
                {
                    frameDigits = frame;
                    frameDigitsRoot = frame;
                    frameDigits = frameDigits->child;
                    if (frameDigits == nullptr) {
                        return;
                    }
                    i++;
                }
            }

            // Find 'digit1'
            if (data.speedoDigits[0] == nullptr)
            {
                if (strstr(name, "digit3"))
                {
                    data.speedoDigits[0] = frame;
                    FRAME_EXTENSION(frame)->LODdist = setLod;
                    i++;
                    continue;
                }
            }
            // Find 'digit2'
            if (data.speedoDigits[1] == nullptr)
            {
                if (strstr(name, "digit2")) 
                {
                    data.speedoDigits[1] = frame;
                    FRAME_EXTENSION(frame)->LODdist = setLod;
                    i++;
                    continue;
                }
            }
            // Find 'digit3'
            if (data.speedoDigits[2] == nullptr)
            {
                if (strstr(name, "digit1")) 
                {
                    data.speedoDigits[2] = frame;
                    FRAME_EXTENSION(frame)->LODdist = setLod;
                    i++;
                    continue;
                }
            }

            frame = frame->next;
            if (frame == nullptr) break;
        }

        if (i == 4) 
        {
            // Set digits to frames
            RpAtomic * tempAtomic;
            RpAtomic * newAtomic;
            for (int i = 0; i < 10; i++)
            {
                tempAtomic = (RpAtomic *)GetFirstObject(frameDigits);
                for (int j = 2; j >= 0; j--)
                {
                    newAtomic = RpAtomicClone(tempAtomic);
                    RpAtomicSetFrame(newAtomic, data.speedoDigits[j]);
                    RpClumpAddAtomic(vehicle->m_pRwClump, newAtomic);
                }
                frameDigits = frameDigits->next;
            }
            DestroyNodeHierarchyRecursive(frameDigitsRoot);
        }
    }
    return;
}



///////////////////////////////////////////// Process Speedo
// TODO: better code, like odometer

void ProcessDigitalSpeedo(CVehicle * vehicle, RwFrame * frame)
{
    VehData &data = vehicleDataMap[vehicle];
    if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
    {
        if (!FRAME_EXTENSION(frame)->flags.bNeverRender)
        {
            RwFrame * frameDigit1 = data.speedoDigits[0];
            RwFrame * frameDigit2 = data.speedoDigits[1];
            RwFrame * frameDigit3 = data.speedoDigits[2];

            CVector vectorSpeed = vehicle->m_vecMoveSpeed;
            float fspeed = vectorSpeed.Magnitude() * 200.0;
            //float fspeed =  / 40.0f;;

            fspeed *= data.speedoMult;

            while (fspeed < 0.0) fspeed *= -1.0;

            //if (useLog) lg << "DigitalSpeedo: speed: " << fspeed << "\n";

            int speedDigit1 = 0, speedDigit2 = 0, speedDigit3 = 0, ispeed = 0;

            if (fspeed > 1.0 || fspeed < -1.0)
            {
                ispeed = (int)fspeed;
                if (ispeed > 999) ispeed = 999;

                string speedDigits = to_string(ispeed);

                int len = speedDigits.length();

                if (len == 1)
                {
                    speedDigit1 = 0;
                    speedDigit2 = 0;
                    speedDigit3 = speedDigits[0] - '0'; // 00X
                }
                if (len == 2)
                {
                    speedDigit1 = 0;
                    speedDigit2 = speedDigits[0] - '0'; // 0X0
                    speedDigit3 = speedDigits[1] - '0'; // 00X
                }
                if (len >= 3)
                {
                    speedDigit1 = speedDigits[0] - '0'; // X00
                    speedDigit2 = speedDigits[1] - '0'; // 0X0
                    speedDigit3 = speedDigits[2] - '0'; // 00X
                }
            }
            //if (useLog) lg << "DigitalSpeedo: " << speedDigit1 << speedDigit2 << speedDigit3 << "\n";

            if (ispeed < 100)
            {
                if (ispeed < 10)
                {
                    HideAllAtomics(frameDigit1);
                    HideAllAtomics(frameDigit2);
                    HideAllAtomicsExcept(frameDigit3, abs(speedDigit3 - 9));
                }
                else
                {
                    HideAllAtomics(frameDigit1);
                    HideAllAtomicsExcept(frameDigit2, abs(speedDigit2 - 9));
                    HideAllAtomicsExcept(frameDigit3, abs(speedDigit3 - 9));
                }
            }
            else
            {
                HideAllAtomicsExcept(frameDigit1, abs(speedDigit1 - 9));
                HideAllAtomicsExcept(frameDigit2, abs(speedDigit2 - 9));
                HideAllAtomicsExcept(frameDigit3, abs(speedDigit3 - 9));
            }
        }
    }
    else
    {
        data.speedoFrame = nullptr;
    }

    return;
}

void SetFrameHierarchyId(RwFrame* pRwFrame, int id) {
    Call::Function<void, RwFrame*, int>(libs.GetSym("_ZN18CVisibilityPlugins19SetFrameHierarchyIdEP7RwFramej"), pRwFrame, id);
}

void SetWheel(RwFrame * frame[6], CVehicle * vehicle)
{
    VehData &data = vehicleDataMap[vehicle];
    for (int j = 0; j < 6; j++)
    {
        if (frame[j])
        {
            if (frame[j]->object.parent && FRAME_EXTENSION(frame[j])->owner == vehicle)
            {
                const string name = GetFrameNodeName(frame[j]);
                //f_wheel 
                for (int i = 8; i <= 13; i++)
                {
                    if (name[i] == '\0' || name[i] == ':' || name[i] == '?') break;
                    if (name[i] == '1')
                    {
                        int wheelId;
                        int subClass = vehicle->m_nVehicleSubClass;

                        switch (i)
                        {
                        case 8:
                            if (subClass == 9 || subClass == 10) wheelId = 4;
                            else wheelId = CAR_WHEEL_RF;
                            break;
                        case 9:
                            if (subClass == 9 || subClass == 10) wheelId = 5;
                            else wheelId = CAR_WHEEL_RB;
                            break;
                        case 10:
                            wheelId = CAR_WHEEL_LB;
                            break;
                        case 11:
                            wheelId = CAR_WHEEL_LF;
                            break;
                        case 12:
                            wheelId = CAR_WHEEL_RM;
                            break;
                        case 13:
                            wheelId = CAR_WHEEL_LM;
                            break;
                        default:
                            wheelId = 0;
                            break;
                        }
                        if (wheelId)
                        {
                            RwFrame * wheelFrame;
                            if (subClass == 9 || subClass == 10) {
                                wheelFrame = CClumpModelInfo::GetFrameFromId(vehicle->m_pRwClump, wheelId);
                            }
                            else
                            {
                                wheelFrame = reinterpret_cast<CAutomobile*>(vehicle)->m_aCarNodes[wheelId]; // this doesn't work for bikes, idkw
                            }

                            if (wheelFrame) {
                                CloneNode(frame[j]->child, vehicle->m_pRwClump, wheelFrame, false, true);
                                SetFrameHierarchyId(frame[j]->child, wheelId);
                            }
                            else
                            {
                            }
                        }
                    }
                }
                DestroyNodeHierarchyRecursive(frame[j]->child);
                RwFrameDestroy(frame[j]);
                continue;
            }
        }
        data.wheelFrame[j] = nullptr;
    }
}

void ProcessSpoiler(CVehicle *vehicle, list<RwFrame*> frames, bool after)
{
    // Process hide spoiler for tuning
    if (!frames.empty())
    {
        bool visible = true;

        if (vehicle->GetUpgrade(6) > 0) //spoiler upgrade
        {
            visible = false;
        }
        else
        {
            if (after)
                visible = true;
            else
                return;
        }

        for (RwFrame *frame : frames)
        {
            if (frame->object.parent && FRAME_EXTENSION(frame)->owner == vehicle)
            {
                if (!visible)
                {
                    HideAllNodesRecursive_Forced(frame, true);
                }
                else {
                    ShowAllNodesRecursive_Forced(frame, true);
                }
            }
            else
            {
                frames.remove(frame);
            }
        }
    }
}

#include "GTASA/CStreaming.h"
#include "GTASA/CTrailer.h"
#include "GTASA/CWorld.h"
#include "GTASA/CFileMgr.h"

unsigned int normalModelIds[] = { 400, 418, 419, 440, 458, 466, 467, 475, 479, 482, 483, 489, 491, 496, 500, 505, 507,
518, 526, 529, 540, 543, 545, 546, 547, 550, 554, 580, 582, 585, 589, 600, 603, 604, 605 };

unsigned int poorfamilyModelIds[] = { 401, 404, 410, 412, 436, 439, 492, 516, 517, 527, 542, 549, 567, 575, 576 };

unsigned int richfamilyModelIds[] = { 405, 421, 426, 445, 474, 477, 535, 551, 558, 559, 560, 561, 562, 565, 566, 579, 587 };

unsigned int executiveModelIds[] = { 402, 409, 411, 415, 429, 434, 451, 480, 506, 533, 534, 536, 541, 555, 602 };

unsigned int workerModelIds[] = { 408, 413, 414, 422, 423, 455, 456, 478, 498, 499, 508, 524, 530, 578, 609 };

struct ModelInfo {
    bool forceTrailer;
    bool enabledTrailer = true || forceTrailer;
};

map<CVehicle*, ModelInfo> modelInfo;

struct MyData {
        unsigned int ModelId;
        unsigned int TrailerIdOne;
        unsigned int TrailerIdTwo;
        unsigned int TrailerIdThree;
        unsigned int TrailerIdFour;
        unsigned int TrailerColours;
        unsigned int TrailerExtras;
        unsigned int TrailerConst;
    };

static vector<MyData>& GetDataVector() {
    static vector<MyData> vec;
    return vec;
}

static void ReadSettingsFile() {
    char path[0xFF];
    sprintf(path, "%sdata/trailer.dat", aml->GetAndroidDataPath());
    ifstream stream(path);
    
    for (string line; getline(stream, line); ) {
        if (line[0] != ';' && line[0] != '#') {
            if (!line.compare("trailer")) {
                while (getline(stream, line) && line.compare("end")) {
                    if (line[0] != ';' && line[0] != '#') {
                        MyData entry;
                        if (sscanf(line.c_str(), "%d, %d, %d, %d, %d, %d, %d, %d", &entry.ModelId, &entry.TrailerIdOne, &entry.TrailerIdTwo, &entry.TrailerIdThree, &entry.TrailerIdFour, &entry.TrailerColours, &entry.TrailerExtras, &entry.TrailerConst) == 8)
                            GetDataVector().push_back(entry);
                    }
                }
            }
        }
    }
}
    
static MyData *GetDataInfoForModel(unsigned int BaseModelId) {
        for (unsigned int i = 0; i < GetDataVector().size(); i++) {
            if (GetDataVector()[i].ModelId == BaseModelId)
                return &GetDataVector()[i];
        }
        return nullptr;
    }
    
void ClearSpaceForMissionEntity(CVector const &pos, CEntity *pEntity) {
    Call::Function<void, CVector const &, CEntity *>(libs.GetSym("_ZN11CTheScripts26ClearSpaceForMissionEntityERK7CVectorP7CEntity"), pos, pEntity);
}

    static void SetTrailer(CVehicle *vehicle, unsigned int modelTrailer, unsigned int colour, unsigned int extra) {
        CStreaming::RequestModel(modelTrailer, 0);
        CStreaming::LoadAllRequestedModels(false);
        if (CStreaming::ms_aInfoForModel[modelTrailer].m_nLoadState == LOADSTATE_LOADED) {
            
            if (extra && vehicle) {
                CVehicleModelInfo::ms_compsToUse[0] = vehicle->m_comp1;
                CVehicleModelInfo::ms_compsToUse[1] = vehicle->m_comp2;
            }
            CVehicle *trailer = nullptr;
            if (CModelInfo::IsVehicleModelType(modelTrailer) == 11)
                trailer = new CTrailer(modelTrailer, 1);
            else
                trailer = new CAutomobile(modelTrailer, 1, true);
            if (trailer) {
                trailer->SetPosn(0.0f, 0.0f, 0.0f);
                trailer->m_info.m_nStatus = 4;
                CWorld::Add(trailer);
                if (vehicle) {
                    trailer->SetTowLink(vehicle, true);
                    if (colour) {
                        trailer->m_nPrimaryColor = vehicle->m_nPrimaryColor;
                        trailer->m_nSecondaryColor = vehicle->m_nSecondaryColor;
                        trailer->m_nTertiaryColor = vehicle->m_nTertiaryColor;
                        trailer->m_nQuaternaryColor = vehicle->m_nQuaternaryColor;
                    }
                }
                ClearSpaceForMissionEntity(trailer->GetPosition(), trailer);
                if (CModelInfo::IsVehicleModelType(modelTrailer) == 11) {
                    trailer->m_nVehicleFlags.bEngineOn = 1;
                    trailer->m_nVehicleFlags.bIsLocked = 1;
                }
                else {
                    trailer->m_nVehicleFlags.bEngineOn = 0;
                    trailer->m_nDoorLock = DOORLOCK_LOCKED;
                    CAutomobile *automobile = reinterpret_cast<CAutomobile *>(trailer);
                    unsigned int perRandomDamage = CGeneral::GetRandomNumberInRange(0, 3);
                    if (perRandomDamage == 2)
                        automobile->SetTotalDamage(1);
                    else if (perRandomDamage == 1)
                        automobile->SetRandomDamage(1);
                }
            }
        }
    }

bool settingsLoaded = false;

DECL_HOOKv(PlayAttachTrailerAudio, CAEVehicleAudioEntity *audio, int soundEvent, float a3) {
        CVehicle *vehicle = reinterpret_cast<CVehicle*>(audio->m_pEntity);
        if ((CTimer::m_snTimeInMilliseconds - vehicle->m_nCreationTime) > 2000) { // don't play it if attached on create
            PlayAttachTrailerAudio(audio, soundEvent, a3);
        }
    }
    
void BreakTowLink(GlossRegisters* regs, PHookHandle hook)
{
    CVehicle *trailer = (CVehicle*)regs->regs.r4;
    CVehicle *tractor = trailer->m_pTractor;
    regs->regs.r0 = (uint32_t)tractor;
    if (tractor) {
                ModelInfo &info = modelInfo[tractor];
                if (info.forceTrailer) {
                    if ((CTimer::m_snTimeInMilliseconds - tractor->m_nCreationTime) > 2000 || (tractor->m_pDriver && tractor->m_pDriver->IsPlayer())) {
                        info.forceTrailer = false;
                    }
                    else {
                        if (tractor->m_pDriver && tractor->m_pDriver->IsPlayer()) {
                            info.forceTrailer = false;
                        }
                        else {
                            // don't break it
                            trailer->SetTowLink(tractor, true);
                            //*(uintptr_t*)(regs.esp - 0x4) = 0x6CF01A;
                        }
                    }
                }
    }
}

static unsigned int Id;
static unsigned int TrailerId;
static unsigned int currentVariant = 0;
    
void ProcessTrailer(CVehicle *vehicle)
{
    //CVehicle *vehicle = FindPlayerVehicle(-1, false);
    
    /*for(int i = 0; i < CPools::ms_pVehiclePool->m_nSize; i++)
    {
        CVehicle *vehicle = CPools::ms_pVehiclePool->GetAt(i);*/
                if (vehicle) {
                    if (vehicle->m_nVehicleFlags.bIsLocked == 1) {
                        if (FindPlayerPed()) {
                            if ((DistanceBetweenPoints(FindPlayerCoors(0), vehicle->GetPosition()) > 200.0f)) {
                                if (vehicle->m_pTrailer) {
                                    vehicle->m_pTrailer->m_nVehicleFlags.bIsLocked = 0;
                                    vehicle->m_pTrailer->CanBeDeleted();
                                }
                                vehicle->m_nVehicleFlags.bIsLocked = 0;
                                vehicle->CanBeDeleted();
                            }
                            else if (FindPlayerPed()->m_pMyVehicle == vehicle) {
                                if (vehicle->m_pTrailer)
                                    vehicle->m_pTrailer->m_nVehicleFlags.bIsLocked = 0;
                                vehicle->m_nVehicleFlags.bIsLocked = 0;
                            }
                        }
                    }
                    MyData *entryModel = GetDataInfoForModel(vehicle->m_nModelIndex);
                    ModelInfo &info = modelInfo[vehicle];
                    if (entryModel && info.enabledTrailer) {
                        if (!entryModel->TrailerConst) {
                            if (currentVariant < 2)
                                currentVariant += 1;
                            else
                                currentVariant = 0;
                            if (currentVariant == 2)
                                info.enabledTrailer = false;
                        }
                        switch (CGeneral::GetRandomNumberInRange(0, 4)) {
                        case 0: Id = entryModel->TrailerIdOne; break;
                        case 1: Id = entryModel->TrailerIdTwo; break;
                        case 2: Id = entryModel->TrailerIdThree; break;
                        case 3: Id = entryModel->TrailerIdFour; break;
                        }
                        
                        switch (Id) {
                            CVehicleModelInfo *vehModel;
                            bool enabledExit;
                        case 0: {
                            enabledExit = false;
                            do {
                                TrailerId = normalModelIds[CGeneral::GetRandomNumberInRange(0, 35)];
                                vehModel = reinterpret_cast<CVehicleModelInfo *>(CModelInfo::ms_modelInfoPtrs[TrailerId]);
                                if (CModelInfo::IsVehicleModelType(TrailerId) == 0 && vehModel->m_nVehicleClass == 0)
                                    enabledExit = true;
                            } while (!enabledExit);
                            break;
                        }
                        case 1: {
                            enabledExit = false;
                            do {
                                TrailerId = poorfamilyModelIds[CGeneral::GetRandomNumberInRange(0, 15)];
                                vehModel = reinterpret_cast<CVehicleModelInfo *>(CModelInfo::ms_modelInfoPtrs[TrailerId]);
                                if (CModelInfo::IsVehicleModelType(TrailerId) == 0 && vehModel->m_nVehicleClass == 1)
                                    enabledExit = true;
                            } while (!enabledExit);
                            break;
                        }
                        case 2: {
                            enabledExit = false;
                            do {
                                TrailerId = richfamilyModelIds[CGeneral::GetRandomNumberInRange(0, 17)];
                                vehModel = reinterpret_cast<CVehicleModelInfo *>(CModelInfo::ms_modelInfoPtrs[TrailerId]);
                                if (CModelInfo::IsVehicleModelType(TrailerId) == 0 && vehModel->m_nVehicleClass == 2)
                                    enabledExit = true;
                            } while (!enabledExit);
                            break;
                        }
                        case 3: {
                            enabledExit = false;
                            do {
                                TrailerId = executiveModelIds[CGeneral::GetRandomNumberInRange(0, 15)];
                                vehModel = reinterpret_cast<CVehicleModelInfo *>(CModelInfo::ms_modelInfoPtrs[TrailerId]);
                                if (CModelInfo::IsVehicleModelType(TrailerId) == 0 && vehModel->m_nVehicleClass == 3)
                                    enabledExit = true;
                            } while (!enabledExit);
                            break;
                        }
                        case 4: {
                            enabledExit = false;
                            do {
                                TrailerId = workerModelIds[CGeneral::GetRandomNumberInRange(0, 15)];
                                vehModel = reinterpret_cast<CVehicleModelInfo *>(CModelInfo::ms_modelInfoPtrs[TrailerId]);
                                if (CModelInfo::IsVehicleModelType(TrailerId) == 0 && vehModel->m_nVehicleClass == 4)
                                    enabledExit = true;
                            } while (!enabledExit);
                            break;
                        }
                        default: TrailerId = Id; break;
                        }
                        if (info.enabledTrailer && !vehicle->m_pTrailer && vehicle->m_nCreatedBy == eVehicleCreatedBy::RANDOM_VEHICLE
                            && (CModelInfo::IsVehicleModelType(TrailerId) == 11 || CModelInfo::IsVehicleModelType(TrailerId) == 0)) {
                            vehicle->m_nVehicleFlags.bMadDriver = 0;
                            vehicle->m_nVehicleFlags.bIsLocked = 1;
                            info.forceTrailer = true;
                            SetTrailer(vehicle, TrailerId, entryModel->TrailerColours, entryModel->TrailerExtras);
                        }
                    }
                    info.enabledTrailer = false;
                }
  //}
}

struct MyDataLight {
        unsigned int ModelId;
    };

static vector<MyDataLight>& GetDataVectorLight() {
    static vector<MyDataLight> vec;
    return vec;
}

static void ReadSettingsFileLight() {
    char path[0xFF];
    sprintf(path, "%sdata/trailerlights.dat", aml->GetAndroidDataPath());
    ifstream stream(path);
    
    for (string line; getline(stream, line); ) {
        if (line[0] != ';' && line[0] != '#') {
            if (!line.compare("modelids")) {
                while (getline(stream, line) && line.compare("end")) {
                    if (line[0] != ';' && line[0] != '#') {
                        MyDataLight entry;
                        if (sscanf(line.c_str(), "%d", &entry.ModelId) == 1)
                            GetDataVectorLight().push_back(entry);
                    }
                }
            }
        }
    }
}

static MyDataLight *GetDataInfoForModelLight(unsigned int BaseModelId) {
        for (unsigned int i = 0; i < GetDataVectorLight().size(); i++) {
            if (GetDataVectorLight()[i].ModelId == BaseModelId)
                return &GetDataVectorLight()[i];
        }
        return nullptr;
    }

void ProcessTrailerLights(CVehicle *vehicle)
{
    // Ensure settings are loaded only once
    static bool settingsLoaded = false;
    if (!settingsLoaded) {
        ReadSettingsFileLight();
        settingsLoaded = true;
    }
    if(vehicle)
    {
        MyDataLight *data = GetDataInfoForModelLight(vehicle->m_nModelIndex);
        if(data)
        {
        
            CVehicle *trailer = vehicle->m_pTrailer;
            if(trailer)
            {
                CAutomobile *mobile = (CAutomobile*)trailer;
                trailer->m_nVehicleFlags.bEngineOn = true;
                    if(vehicle->m_nVehicleFlags.bLightsOn || (vehicle->m_renderLights.m_bRightRear && vehicle->m_renderLights.m_bLeftRear))
                    {
                        mobile->m_damageManager.SetLightStatus(LIGHT_FRONT_LEFT, VEHICLE_LIGHT_SMASHED);
                        mobile->m_damageManager.SetLightStatus(LIGHT_FRONT_RIGHT, VEHICLE_LIGHT_SMASHED);
                        trailer->m_nOverrideLights = FORCE_CAR_LIGHTS_ON;
                        trailer->m_renderLights.m_bRightRear = true;
                        trailer->m_renderLights.m_bLeftRear = true;
                    }
                    else
                    {
                        trailer->m_nOverrideLights = NO_CAR_LIGHT_OVERRIDE;
                        trailer->m_renderLights.m_bRightRear = false;
                        trailer->m_renderLights.m_bLeftRear = false;
                        mobile->m_damageManager.SetLightStatus(LIGHT_FRONT_LEFT, VEHICLE_LIGHT_OK);
                        mobile->m_damageManager.SetLightStatus(LIGHT_FRONT_RIGHT, VEHICLE_LIGHT_OK);
                    }
            }
        }
      
        if(vehicle->m_nVehicleSubClass == VEHICLE_TRAILER)
        {
            if(!vehicle->m_pTractor) //m_pAttachToEntity
            {
                vehicle->m_nOverrideLights = NO_CAR_LIGHT_OVERRIDE;
                vehicle->m_nVehicleFlags.bEngineOn = false;
                vehicle->m_renderLights.m_bRightRear = false;
                vehicle->m_renderLights.m_bLeftRear = false;
            }
        }
    }
}



#include <random>

// Declare a random number generation function
int Random(int min, int max) {
    // Create a static random device and generator to ensure it's initialized only once
    static std::random_device rd;
    static std::mt19937 gen(rd());

    // Create a uniform distribution for the specified range [min, max]
    std::uniform_int_distribution<> distrib(min, max);

    // Return a random number in the given range
    return distrib(gen);
}


void CVisibilityPlugins_SetAtomicFlag(RpAtomic* pRpAtomic, int flag) {
    Call::Function<void, RpAtomic*, int>(libs.GetSym("_ZN18CVisibilityPlugins13SetAtomicFlagEP8RpAtomict"), pRpAtomic, flag);
}

void FrameRenderAlways(RwFrame *frame)
{
    if (!rwLinkListEmpty(&frame->objectList))
    {
        RwObjectHasFrame * atomic;

        RwLLLink * current = rwLinkListGetFirstLLLink(&frame->objectList);
        RwLLLink * end = rwLinkListGetTerminator(&frame->objectList);

        current = rwLinkListGetFirstLLLink(&frame->objectList);
        while (current != end) {
            atomic = rwLLLinkGetData(current, RwObjectHasFrame, lFrame);
            CVisibilityPlugins_SetAtomicFlag((RpAtomic*)atomic, 0x400);
            atomic->object.flags |= 0x400; // set
            //atomic->object.flags &= ~0x4; // clear

            current = rwLLLinkGetNext(current);
        }
    }
    return;
}

#include "GTASA/CStreaming.h"
// Disable warnings (caution!)
#pragma warning( disable : 4244 ) // data loss

uint32_t txdIndexStart = 2000;
bool bIndieVehicles;

int& m_nCurrentZoneType = *(int*)libs.GetSym("_ZN9CPopCycle18m_nCurrentZoneTypeE");
unsigned char &ms_nGameClockHours = *(unsigned char *)libs.GetSym("_ZN6CClock18ms_nGameClockHoursE");

bool IsPlayerOnAMission() {
    return Call::Function<bool>(libs.GetSym("_ZN11CTheScripts18IsPlayerOnAMissionEv"));
}

/*
bool FindZone(CVector* point, int zonename_part1 ,int zonename_part2, eZoneType type)
{
    return Call::Function<bool, CVector*, int, int, eZoneType>(libs.GetSym("point, zonename_part1, zonename_part2, type);
}*/

void FindVehicleCharacteristicsFromNode(RwFrame * frame, CVehicle * vehicle, bool bReSearch) 
{
    const string name = GetFrameNodeName(frame);
    size_t found;

    //if (useLog) lg << "Charac: Processing node: " << name << "\n";

    found = name.find("="); // For performance
    if (found != string::npos)
    {
        if (!bReSearch)
        {
            // Paintjob
            found = name.find("_pj=");
            if (found != string::npos)
            {
                int paintjob;
                bool preserveColor = false;
                int digit1 = name[found + 4] - '0';

                VehData &xdata = vehicleDataMap[vehicle];

                if (name[found + 5] == '-')
                {
                    int digit2 = name[found + 6] - '0';

                    srand((xdata.randomSeed + xdata.randomSeedUsage));
                    xdata.randomSeedUsage++;

                    paintjob = Random(digit1, digit2);
                    //if (useLog) lg << "Charac: Found 'pj' (paintjob). Calculated from '-' result '" << paintjob << "' at '" << name << "'\n";
                    if (name[found + 7] == 'c') preserveColor = true;
                }
                else
                {
                    if (name[found + 5] == 'c') preserveColor = true;
                    paintjob = digit1;
                    //if (useLog) lg << "Charac: Found 'pj' (paintjob). Set '" << digit1 << "' at '" << name << "'\n";
                }

                xdata.paintjob = paintjob;
                xdata.flags.bPreservePaintjobColor = preserveColor;
            }

            // Colors
            found = name.find("_cl=");
if (found != string::npos)
{
    VehData &xdata = vehicleDataMap[vehicle];
    int j = 0;
    
    // Extract the substring that contains the color data
    string colorString = name.substr(found + 4);
    size_t start = 0;
    size_t end = 0;

    // Loop through the colorString to extract each number separated by commas
    while ((end = colorString.find(',', start)) != string::npos) {
        try {
            // Convert the substring between 'start' and 'end' to an integer
            int num = std::stoi(colorString.substr(start, end - start));
            xdata.color[j] = num;
        } catch (const std::exception &) {
            xdata.color[j] = -1;
        }
        start = end + 1;
        j++;
    }

    // Handle the last number (or only number if no commas were found)
    try {
        int num = std::stoi(colorString.substr(start));
        xdata.color[j] = num;
    } catch (const std::exception &) {
        xdata.color[j] = -1;
    }

    // if (useLog) lg << "Charac: Found 'cl' (colors) " << xdata.color[0] << " " << xdata.color[1] << " " << xdata.color[2] << " " << xdata.color[3] << "\n";
}


            // Driver
found = name.find("_drv=");
if (found != string::npos)
{
    list<int> driverList;

    // Extract the substring that contains the driver list
    string driverString = name.substr(found + 5);
    size_t start = 0;
    size_t end = 0;

    // Parse the driver list, separated by commas
    while ((end = driverString.find(',', start)) != string::npos) {
        try {
            // Convert the substring between 'start' and 'end' to an integer
            int num = std::stoi(driverString.substr(start, end - start));
            driverList.push_back(num);
        } catch (const std::exception &) {
            break; // Stop parsing if there's an error
        }
        start = end + 1;
    }

    // Process the last (or only) driver model number
    try {
        int num = std::stoi(driverString.substr(start));
        driverList.push_back(num);
    } catch (const std::exception &) {
        // Do nothing, error handling already handled with break in the loop
    }

    VehData &xdata = vehicleDataMap[vehicle];

    // Random selection logic
    srand((xdata.randomSeed + xdata.randomSeedUsage));
    xdata.randomSeedUsage++;

    int rand = Random(0, (driverList.size() - 1));

    list<int>::iterator it = driverList.begin();
    advance(it, rand);
    int driverModel = *it;

    // if (useLog) lg << "Charac: Found 'drv' (driver), selected '" << driverModel << "' at '" << name << "'\n";
    xdata.driverModel = driverModel;
}


           // Occupants
found = name.find("_oc=");
if (found != string::npos)
{
    VehData &xdata = vehicleDataMap[vehicle];

    // Extract the substring that contains the occupant models
    string occupantsString = name.substr(found + 4);
    size_t start = 0;
    size_t end = 0;

    // Parse the occupant models, separated by commas
    while ((end = occupantsString.find(',', start)) != string::npos) {
        try {
            // Convert the substring between 'start' and 'end' to an integer
            int num = std::stoi(occupantsString.substr(start, end - start));
            xdata.occupantsModels.push_back(num);
        } catch (const std::exception &) {
            break; // Stop parsing if there's an error
        }
        start = end + 1;
    }

    // Process the last (or only) occupant model number
    try {
        int num = std::stoi(occupantsString.substr(start));
        xdata.occupantsModels.push_back(num);
    } catch (const std::exception &) {
        // Do nothing, error handling already handled with break in the loop
    }

    // if (useLog) lg << "Charac: Found 'oc' (occupants), variations " << xdata.occupantsModels.size() << " at '" << name << "'\n";

    // Check for the '.' and assign the passAddChance
    found = name.find(".");
    if (found != string::npos)
    {
        xdata.passAddChance = name[found + 1] - '0';
    }
}

/*
            // MDPM custom chances
            found = name.find("_mdpmnpc=");
            if (found != string::npos)
            {
                int i = 9;
                int num = stoi(&name[found + i]);

                i++;
                do {
                    i++;
                } while (name[found + i] != ',');

                i++;
                float minVol = stof(&name[found + i]);
                i++;

                do {
                    i++;
                } while (name[found + i] != '-');
                i++;

                float maxVol = stof(&name[found + i]);

                //if (useLog) lg << "MDPM: Found '_mdpmnpc' (MDPM NPC) chances '" << num << " vol " << minVol << "-" << maxVol << "'\n";
                ExtendedData &xdata = xData.Get(vehicle);
                xdata.mdpmCustomChances = num;
                xdata.mdpmCustomMinVol = minVol;
                xdata.mdpmCustomMaxVol = maxVol;
            }

            // Sound engine
            found = name.find("_se=");
            if (found != string::npos)
            {
                int i = 4;
                int sound1 = stoi(&name[found + i]);

                do {
                    ++i;
                    if ((found + i) >= 22) {
                        break;
                    }
                } while (name[found + i] != ',');

                if ((found + i) <= 22) {
                    ++i;
                    int sound2 = stoi(&name[found + i]);
                    if (useLog) lg << "Charac: Found '_se=' (sound engine) to sound 1 '" << sound1 << "' sound 2 '" << sound2 << "'\n";
                    vehicle->m_vehicleAudio.m_nEngineAccelerateSoundBankId = sound1;
                    vehicle->m_vehicleAudio.m_nEngineDecelerateSoundBankId = sound2;

                    int soundId = 0;
                    do
                        CVehicle__CancelVehicleEngineSound(&vehicle->m_vehicleAudio, soundId++);
                    while (soundId < 12);
                    CAEVehicleAudioEntity__StoppedUsingBankSlot(vehicle->m_vehicleAudio.m_nEngineBankSlotId);
                    vehicle->m_vehicleAudio.m_nEngineBankSlotId = CAEVehicleAudioEntity__RequestBankSlot(sound2);
                }
                else {
                    if (useLog) lg << "Charac: ERROR '_se=' (sound engine) can't read values vehicle ID " << vehicle->m_nModelIndex << "\n";
                }
            }*/

            // Dirty (drt=5-9)
            found = name.find("_drt=");
            if (found != string::npos)
            {
                float fdigit1;
                float fdigit2;
                float dirtyLevel;

                VehData &xdata = vehicleDataMap[vehicle];

                int digit1 = name[found + 5] - '0';

                fdigit1 = digit1 * 1.6666f;

                if (name[found + 6] == '-')
                {
                    int digit2 = name[found + 7] - '0';
                    fdigit2 = digit2 * 1.6666f;

                    srand((xdata.randomSeed + xdata.randomSeedUsage));
                    xdata.randomSeedUsage++;

                    dirtyLevel = Random(fdigit1, fdigit2);
                    //if (useLog) lg << "Charac: Found 'drt' (dirty). Calculated from '-' result '" << dirtyLevel << "' at '" << name << "'\n";
                }
                else
                {
                    dirtyLevel = fdigit1;
                    //if (useLog) lg << "Charac: Found 'drt' (dirty). Set '" << dirtyLevel << "' at '" << name << "'\n";
                }

                xdata.dirtyLevel = dirtyLevel;
            }
        }

        // Double exhaust smoke
        found = name.find("_dexh=");
        if (found != string::npos)
        {
            //if (useLog) lg << "Charac: Found 'dexh' (double exhaust) at '" << name << "'\n";
            VehData &xdata = vehicleDataMap[vehicle];
            int enable = name[found + 6] - '0';
            if (enable) xdata.doubleExhaust = true;
            else xdata.doubleExhaust = false;
        }

        // Swinging chassis
        found = name.find("_swc=");
        if (found != string::npos)
        {
            //if (useLog) lg << "Charac: Found 'swc' (swinging chassis) at '" << name << "'\n";
            VehData &xdata = vehicleDataMap[vehicle];
            int enable = name[found + 5] - '0';
            if (enable) xdata.swingingChassis = true;
            else xdata.swingingChassis = false;
        }

        // Body tilt (not finished)
        /*found = name.find("_btilt=");
        if (found != string::npos)
        {
            if (useLog) lg << "Charac: Found 'btilt' (body tilt) at '" << name << "'\n";
            ExtendedData &xdata = xData.Get(vehicle);
            xdata.bodyTilt = stof(&name[found + 7]);
        }*/

    }

    // LOD less
    found = name.find("<");
    if (found != string::npos)
    {
        VehData &xdata = vehicleDataMap[vehicle];
        int num = name[found + 1] - '0';
        if (num != 0) num *= -1;
        FRAME_EXTENSION(frame)->LODdist = num;
        //FrameRenderAlways(frame);
        //if (useLog) lg << "LOD: Found '<' level " << num << " at '" << name << "'\n";
    }

    // LOD greater
    found = name.find(">");
    if (found != string::npos)
    {
        VehData &xdata = vehicleDataMap[vehicle];
        int num = name[found + 1] - '0';
        FRAME_EXTENSION(frame)->LODdist = num;
        FrameRenderAlways(frame);
        //if (useLog) lg << "LOD: Found '>' level " << num << " at '" << name << "'\n";
    }

    // Patches: Bus render
    found = name.find("_busrender");
    if (found != string::npos)
    {
        //if (useLog) lg << "Charac: Found 'busrender' (force bus render) at '" << name << "'\n";
        VehData &xdata = vehicleDataMap[vehicle];
        xdata.flags.bBusRender = true;

        // Make driver be rendered in bus if it's already inside it (created in same frame)
        CPed *busDriver = vehicle->m_pDriver;
        if (busDriver) { busDriver->m_nPedFlags.bRenderPedInCar = true; }
    }

    return;
}

///////////////////////////////////////////// Set Characteristics

/*void SetCharacteristicsInSetModel(CVehicle * vehicle, bool bReSearch)
{
    ExtendedData &xdata = xData.Get(vehicle);

    return;
}*/

void SetCharacteristicsInRender(CVehicle * vehicle, bool bReSearch)
{
    VehData &xdata = vehicleDataMap[vehicle];

    if (!bReSearch) 
    {
        // Colors
        if (xdata.color[0] >= 0) 
        {
           // if (useLog) lg << "Charac: Applying color 1: " << xdata.color[0] << "\n";
            vehicle->m_nPrimaryColor = xdata.color[0];
        }
        if (xdata.color[1] >= 0) 
        {
           // if (useLog) lg << "Charac: Applying color 2: " << xdata.color[1] << "\n";
            vehicle->m_nSecondaryColor = xdata.color[1];
        }
        if (xdata.color[2] >= 0) 
        {
           // if (useLog) lg << "Charac: Applying color 3: " << xdata.color[2] << "\n";
            vehicle->m_nTertiaryColor = xdata.color[2];
        }
        if (xdata.color[3] >= 0) 
        {
           // if (useLog) lg << "Charac: Applying color 4: " << xdata.color[3] << "\n";
            vehicle->m_nQuaternaryColor = xdata.color[3];
        }

        // Paintjob
        if (xdata.paintjob >= 0) 
        {
            if ((int)xdata.paintjob > 0) {
               // if (useLog) lg << "Charac: Applying paintjob: " << (int)xdata.paintjob << " keep color " << (bool)xdata.flags.bPreservePaintjobColor << "\n";
                vehicle->m_nVehicleFlags.bDontSetColourWhenRemapping = xdata.flags.bPreservePaintjobColor;
                vehicle->SetRemap(xdata.paintjob - 1);
                if (vehicle->m_nRemapTxd >= 0)
                {
                    CStreaming::RequestModel(vehicle->m_nRemapTxd + txdIndexStart, (eStreamingFlags::KEEP_IN_MEMORY | eStreamingFlags::PRIORITY_REQUEST));
                    CStreaming::LoadAllRequestedModels(true);
                }
            }
        }

        // Dirty
        if (xdata.dirtyLevel >= 0.0) 
        {
           // if (useLog) lg << "Charac: Applying dirty: " << xdata.dirtyLevel << "\n";
            vehicle->m_fDirtLevel = xdata.dirtyLevel;
        }

        /*
        // For only random created vehicles
        if (vehicle->m_nCreatedBy == eVehicleCreatedBy::RANDOM_VEHICLE) 
        {
            // Custom Driver
            if (xdata.driverModel > 0) 
            {
                CPed * driver = vehicle->m_pDriver;
                if (driver && driver->m_nModelIndex != xdata.driverModel)
                {
                    char *a = (char*)driver;
                    if (a[0x484] == 1) //Createdby RANDOM
                    { 
                        int driverModel = xdata.driverModel;
                        if (useLog) lg << "Charac: Changing driver: " << driverModel << "\n";
                        ChangePedModel(driver, driverModel, vehicle, -1);
                    }
                }
            }

            // Custom Passengers
            if (!xdata.occupantsModels.empty()) 
            {
                CPed *driver = vehicle->m_pDriver;
                if (driver) 
                {
                    char *a = (char*)driver;
                    if (a[0x484] == 1) //Createdby RANDOM
                    { 
                        if (xdata.driverModel <= 0) 
                        {
                            srand((xdata.randomSeed + xdata.randomSeedUsage));
                            xdata.randomSeedUsage++;

                            int rand = Random(0, (xdata.occupantsModels.size() - 1));

                            list<int>::iterator it = xdata.occupantsModels.begin();
                            advance(it, rand);
                            int model = *it;

                            if (useLog) lg << "Charac: Changing driver from '_oc': " << model << "\n";

                            ChangePedModel(driver, model, vehicle, -1);
                        }

                        int maxPassengers = vehicle->m_nMaxPassengers;
                        bool isTaxi = false;
                        if (maxPassengers > 1) {
                            if (xdata.taxiSignMaterial || vehicle->m_nModelIndex == eModelID::MODEL_TAXI || vehicle->m_nModelIndex == eModelID::MODEL_CABBIE) {
                                isTaxi = true;
                            }
                        }
                        for (int i = 0; i < maxPassengers; i++) 
                        {
                            CPed *pass = vehicle->m_apPassengers[i];
                            if (!pass)
                            {
                                int rand = Random(0, 9);
                                int chances = xdata.passAddChance;
                                if (maxPassengers > 1) {
                                    if (isTaxi) {
                                        if (i == 0) continue; // don't create front seat if taxi
                                    }
                                    else { // not taxi, increase chances for front seat
                                        if (i == 0) chances *= 2;
                                        else chances /= 2;
                                    }
                                }
                                if (rand > chances)
                                {
                                    //if (useLog) lg << "Charac: No passenger added because of chance " << (int)xdata.passAddChance << "\n";
                                    continue;
                                }
                            }
                            else 
                            {
                                char *a = (char*)pass;
                                if (a[0x484] != 1) //Createdby NOT RANDOM
                                { 
                                    //if (useLog) lg << "Charac: No passenger changed: not random created\n";
                                    continue;
                                }
                            }

                            int model = 0;
                            int tries = 0;
                            do {
                                int rand = Random(0, (xdata.occupantsModels.size() - 1));

                                list<int>::iterator it = xdata.occupantsModels.begin();
                                advance(it, rand);
                                model = *it;

                                if (i != 0) break; // Don't do this for back seats

                                tries++;
                                if (tries > 5) break;
                                
                            } while (model == driver->m_nModelIndex);

                            if (pass) 
                            {
                                if (useLog) lg << "Charac: Changing passenger " << (i + 1) << " model " << model << "\n";
                                ChangePedModel(pass, model, vehicle, i);
                            }
                            else
                            {
                                if (useLog) lg << "Charac: Adding passenger " << (i + 1) << " model " << model << "\n";
                                if (LoadModel(model))
                                {
                                    CVector pos;
                                    pos.x = vehicle->m_placement.m_vPosn.x;
                                    pos.y = vehicle->m_placement.m_vPosn.y;
                                    pos.z = vehicle->m_placement.m_vPosn.z;
                                    CPedModelInfo *modelInfo = (CPedModelInfo *)CModelInfo::GetModelInfo(model);
                                    if (useLog) lg << "Charac: creating ped " << (i + 1) << " model " << model << "\n";
                                    pass = CPopulation::AddPed((ePedType)modelInfo->m_nPedType, model, pos, 0);
                                    if (useLog) lg << "Charac: ped created, get door " << (i + 1) << " model " << model << "\n";
                                    int doorNodeId = CCarEnterExit::ComputeTargetDoorToEnterAsPassenger(vehicle, i);
                                    CCarEnterExit::SetPedInCarDirect(pass, vehicle, doorNodeId, false);
                                    MarkModelAsNoLongerNeeded(model);
                                }
                                else if (useLog) lg << "ERROR: Model doesn't exist! " << model << "\n";
                            }
                        }
                    }
                }
            }
        }*/

    }

}

int FindNextInterrogationMark(const string str, int from, int len)
{
    while (from < len)
    {
        from++;
        if (str[from] == '?' || str[from] == ':') return from;
    } 
    return len;
}

bool ClassConditionsValid(const string nodeName, int from, CVehicle *vehicle)
{
    int next = 0;
    string str;
    char zoneName[8];
    int len = nodeName.length();

    // For optimzation: faster and common functions before, slower ones at bottom
    // Never return false during checks, just keep it
    // Think twice using else to next condition
    while (from < len)
    {
        from++;

        // city
        if (nodeName[from] == 'c')
        {
            from++;
            int i = nodeName[from] - '0';
            if (CWeather::WeatherRegion == i) return true;
            from++;
        }

        // popcycle
        if (nodeName[from] == 'p')
        {
            from++;
            if (m_nCurrentZoneType == stoi(&nodeName[from])) return true;
            from++;
        }

        // driver pedstat
        if (nodeName[from] == 'd')
        {
            from++;
            if (vehicle->m_pDriver) {
                int pedpedstatId = *(uint32_t*)(vehicle->m_pDriver->m_pStats);
                if (vehicle->m_pDriver->m_nModelIndex == MODEL_BFOST) pedpedstatId = 24; // by default BFOST is STAT_STREET_GIRL, consider STAT_OLD_GIRL
                if (pedpedstatId == stoi(&nodeName[from])) return true;
            }
            from++;
        }

        // no driver pedstat
        if (nodeName[from] == 'n' && nodeName[from + 1] == 'd')
        {
            from += 2;
            if (!vehicle->m_pDriver) return true;
            int pedpedstatId = *(uint32_t*)(vehicle->m_pDriver->m_pStats);
            if (vehicle->m_pDriver->m_nModelIndex == 10) pedpedstatId = 24; // by default BFOST is STAT_STREET_GIRL, consider STAT_OLD_GIRL
            if (pedpedstatId != stoi(&nodeName[from])) return true;
            from++;
        }

        // mission controlled
        if (nodeName[from] == 'm')
        {
            from++;
            if (vehicle->m_nCreatedBy == eVehicleCreatedBy::MISSION_VEHICLE && IsPlayerOnAMission()) return true;
        }

        // not mission controlled
        if (nodeName[from] == 'n' && nodeName[from + 1] == 'm')
        {
            from += 2;
            if (vehicle->m_nCreatedBy != eVehicleCreatedBy::MISSION_VEHICLE || !IsPlayerOnAMission()) return true;
            from++;
        }

        // rain
        if (nodeName[from] == 'r' && nodeName[from + 1] == 'a' && nodeName[from + 2] == 'i' && nodeName[from + 3] == 'n') 
        {
            if (CWeather::OldWeatherType == 8 || CWeather::OldWeatherType == 16 ||
                CWeather::NewWeatherType == 8 || CWeather::NewWeatherType == 16 ||
                CWeather::ForcedWeatherType == 8 || CWeather::ForcedWeatherType == 16)
            {
                //if (useLog) lg << "RAIN OK \n";
                return true;
            }
            //else if (useLog) lg << "NOT RAIN \n";
            from += 4;
        }

        // norain
        if (nodeName[from] == 'n' && nodeName[from + 1] == 'o' && nodeName[from + 2] == 'r' && nodeName[from + 3] == 'a' && nodeName[from + 4] == 'i' && nodeName[from + 5] == 'n')
        {
            if (CWeather::OldWeatherType != 8 && CWeather::OldWeatherType != 16 &&
                CWeather::NewWeatherType != 8 && CWeather::NewWeatherType != 16 &&
                CWeather::ForcedWeatherType != 8 && CWeather::ForcedWeatherType != 16)
            {
                //if (useLog) lg << "NOT RAIN OK \n";
                return true;
            }
            //else if (useLog) lg << "NOT NOT RAIN \n";
            from += 6;
        }

        // hour
        if (nodeName[from] == 'h') 
        {
            from++;
            int minHour;
            int maxHour;

            minHour = stoi(&nodeName[from]);
            if (minHour > 9)
                from += 1;
            else
                from += 2;
            maxHour = stoi(&nodeName[from]);

            //if (useLog) lg << minHour << " " << maxHour << "\n";

            if (maxHour < minHour)
            {
                if (ms_nGameClockHours >= minHour || ms_nGameClockHours <= maxHour) return true;
            }
            else
            {
                if (ms_nGameClockHours >= minHour && ms_nGameClockHours <= maxHour) return true;
            }

            from++;
        }

        /*// zone
        if (nodeName[from] == 'z')
        {
            from++;
            next = FindNextInterrogationMark(nodeName, from, len);
            str = nodeName.substr(from, next);
            memset(zoneName, 0, 8);
            strncpy(zoneName, &str[0], next - from);
            //if (useLog) lg << "zone value " << zoneName << "\n";
            from = next;
            if (CTheZones::FindZone(&vehicle->GetPosition(), *(int*)zoneName, *(int*)(zoneName + 4), eZoneType::ZONE_TYPE_NAVI))
            {
                //if (useLog) lg << "ZONE OK \n";
                return true;
            }
            //else if (useLog) lg << "NOT ZONE \n";
        }*/
    }
    return false;
}

void ProcessClassesRecursive(RwFrame * frame, CVehicle * vehicle, bool bReSearch, bool bSubclass)
{
    VehData &xdata = vehicleDataMap[vehicle];

    string name = GetFrameNodeName(frame);
    name = name.substr(0, 24);
    size_t found;

    vector<RwFrame*> classNodes;
    vector<int> classNodesPercent;

    found = name.find("_reset");
    if (found != string::npos)
    {
        //if (useLog) lg << "Extras: Reseting classes \n";
        list<string> &classList = getClassList();
        classList.clear();
    }

    RwFrame * tempNode = frame->child;
    if (tempNode) 
    {
        if (!bSubclass)
        {
            srand((xdata.randomSeed + xdata.randomSeedUsage));
            xdata.randomSeedUsage++;
        }

        // Get properties
        int selectVariations = 1;
        bool randomizeVariations = false;

        size_t found = name.find(":");
        if (found != string::npos)
        {
            if (name.length() > found)
            {
                selectVariations = stoi(&name[found + 1]);
                int i = 0;
                do {
                    if (name.length() > (found + 1 + i))
                    {
                        if (name[found + 2 + i] == '+') randomizeVariations = true;
                    }
                    i++;
                } while (i <= 1);
            }
        }

        // Count classes
        bool isAnyConditionClass = false;
        int random = 0;
        int totalClass = 0;
        while (tempNode) 
        {
            string tempNodeName = GetFrameNodeName(tempNode);
            tempNodeName = tempNodeName.substr(0, 24);

            if (tempNodeName[0] == '!')
            {
                RwFrame *tempCharacFrame = tempNode->child;
                while (tempCharacFrame)
                {
                    FindVehicleCharacteristicsFromNode(tempCharacFrame, vehicle, bReSearch);
                    tempCharacFrame = tempCharacFrame->next;
                }
                tempNode = tempNode->next;
                continue;
            }

            found = tempNodeName.find_first_of("?");
            if (found != string::npos)
            {
                //if (useLog) lg << "Found '?' condition at '" << tempNodeName << "'\n";
                if (!ClassConditionsValid(tempNodeName, found, vehicle)) {
                    tempNode = tempNode->next;
                    //if (useLog) lg << "Condition check failed \n";
                    continue;
                }
                else {
                    found = tempNodeName.find("[");
                    if (found != string::npos)
                    {
                        int percent = stoi(&tempNodeName[found + 1]);
                        if (percent != 100)
                        {
                            int randomPercent = Random(1, 100);
                            if (percent < randomPercent)
                            {
                                //if (useLog) lg << "Class condition: " << tempNodeName << " not added due to percent\n";
                                tempNode = tempNode->next;
                                continue;
                            }
                        }
                    }
                    if (!isAnyConditionClass) {
                        // Clear all other classes, only consider condition classes
                        if (classNodes.size() > 0)
                        {
                            totalClass = 0;
                            classNodes.clear();
                            classNodesPercent.clear();
                        }
                        isAnyConditionClass = true;
                    }
                    //if (useLog) lg << "Class added for checking: " << tempNodeName << "\n";
                    classNodesPercent.push_back(100);
                    classNodes.push_back(tempNode);
                    totalClass++;
                }
            }
            else
            {
                // Don't add new classes if there is any condition class
                if (!isAnyConditionClass) {
                    found = tempNodeName.find("[");
                    if (found != string::npos)
                    {
                        int percent = stoi(&tempNodeName[found + 1]);
                        classNodesPercent.push_back(percent);
                    }
                    else
                    {
                        classNodesPercent.push_back(100);
                    }
                    //if (useLog) lg << "Class added for checking: " << tempNodeName << "\n";
                    classNodes.push_back(tempNode);
                    totalClass++;
                }
            }
            tempNode = tempNode->next;
            continue;
        }

        //if (useLog) lg << "Total classes is: " << totalClass << "\n";

        if (totalClass > 0) 
        {
            // Randomize (:+) or just fix max
            if (randomizeVariations == true)
                selectVariations = Random(selectVariations, totalClass);
            else
                if (selectVariations > totalClass) selectVariations = totalClass;

            // Insert classes
            int i = 0;
            do {
                random = Random(0, totalClass - 1);

                if (classNodes[random] == nullptr) 
                {
                    continue;
                }
                else
                {
                   // if (useLog) lg << "Select " << random << " from max " << (totalClass - 1) << " is " << GetFrameNodeName(classNodes[random]) << "\n";
                    if (classNodesPercent[random] != 100)
                    {
                        int randomPercent = Random(1, 100);
                        if (classNodesPercent[random] < randomPercent)
                        {
                            //if (useLog) lg << "Extras: Class " << GetFrameNodeName(classNodes[random]) << " not selected: " << classNodesPercent[random] << " percent\n";
                            continue;
                        }
                    }

                    FindVehicleCharacteristicsFromNode(classNodes[random], vehicle, bReSearch);
                    if (classNodes[random]->child)
                    {
                        ProcessClassesRecursive(classNodes[random], vehicle, bReSearch, true);
                    }

                    string className = GetFrameNodeName(classNodes[random]);
                    className = className.substr(0, 24);

                    list<string> &classList = getClassList();

                    size_t found1 = className.find_first_of("_");
                    size_t found2 = className.find_first_of("[");
                    size_t found3 = className.find_first_of(":");
                    size_t found4 = className.find_first_of("?");
                    if (found1 != string::npos || found2 != string::npos || found3 != string::npos || found4 != string::npos)
                    {
                        size_t cutPos;

                        if (found1 < found2)
                            cutPos = found1;
                        else
                            cutPos = found2;

                        if (found3 < cutPos)
                            cutPos = found3;

                        if (found4 < cutPos)
                            cutPos = found4;

                        string classNameFixed = className.substr(0, cutPos);
                        if (classNameFixed.length() > 0)
                        {
                            classList.push_back(classNameFixed);
                            //if (useLog) lg << "Extras: Class inserted: " << classNameFixed << "\n";
                        }
                    }
                    else 
                    {
                        classList.push_back(className);
                        //if (useLog) lg << "Extras: Class inserted: " << className << "\n";
                    }

                    classNodes[random] = nullptr;
                }
                xdata.classFrame = classNodes[random];
                i++;
            } while (i < selectVariations);
        }
    }
}

///////////////////////////////////////////// Process Extras

bool IsDamAtomic(RpAtomic *a)
{
    uint8_t *b = reinterpret_cast<uint8_t*>(a);
    int atomicPluginOffset = *(int*)libs.GetSym("_ZN18CVisibilityPlugins21ms_atomicPluginOffsetE");
    uint16_t flags = *reinterpret_cast<uint16_t*>(b + atomicPluginOffset + 2);
    return (flags & 2) != 0;
}

bool IsOkAtomic(RpAtomic *a)
{
    uint8_t *b = reinterpret_cast<uint8_t*>(a);
    int atomicPluginOffset = *(int*)libs.GetSym("_ZN18CVisibilityPlugins21ms_atomicPluginOffsetE");
    uint16_t flags = *reinterpret_cast<uint16_t*>(b + atomicPluginOffset + 2);
    return (flags & 1) != 0;
}

void RemoveFrameClassFromNormalArray(RwFrame * frameClass, RwFrame * frameArray[])
{
    for (int i = 0; i < 32; i++)
    {
        if (frameArray[i] == frameClass) { frameArray[i] = nullptr; return; }
    }
    return;
}


bool FrameIsOtherClass(RwFrame * frame)
{
    string name = GetFrameNodeName(frame);
    name = name.substr(0, 24);
    size_t found = name.find("[");
    if (found != string::npos) 
    {
        list<string> &classList = getClassList();
        if (classList.size() > 0) 
        {
            for (list<string>::iterator it = classList.begin(); it != classList.end(); ++it) 
            {
                size_t foundclass = name.find(*it);
                if (foundclass != string::npos) 
                {
                    return false;
                }
            }
        }
        return true;
    }
    return false;
}

void ProcessExtraRecursive(RwFrame * frame, CVehicle * vehicle) 
{
    string name = GetFrameNodeName(frame);
    name = name.substr(0, 24);
   // if (useLog) lg << "Extras: Processing node: '" << name << "'\n";
    
    RwFrame * tempFrame = frame->child;
    if (tempFrame != nullptr)
    {

        // -- Get properties
        int selectVariations = -1;
        bool randomizeVariations = false;

        size_t found = name.find(":");
        if (found != string::npos) 
        {
            if (name.length() > found) 
            {
                selectVariations = stoi(&name[found + 1]);
                int i = 0;
                do {
                    if (name.length() > (found + 1 + i)) 
                    {
                        if (name[found + 2 + i] == '+') 
                        {
                            randomizeVariations = true;
                        }
                    }
                    i++;
                } while (i <= 1);
            }
        }


        // -- Store extra frames
        int frames = 0;
        int classFrames = 0;
        RwFrame * extraFrames[33];
        RwFrame * extraFramesMatchClass[33];
        memset(extraFrames, 0, sizeof(extraFrames));
        memset(extraFramesMatchClass, 0, sizeof(extraFramesMatchClass));


        // -- List all extra frames
        while (frames < 32) 
        {
            if (tempFrame != nullptr) 
            {
                // Store class match frames
                list<string> &classList = getClassList();
                if (classList.size() > 0) 
                {
                    // If frame has class
                    string tempFrameName = GetFrameNodeName(tempFrame);
                    tempFrameName = tempFrameName.substr(0, 24);
                    int startStringSearchIndex = 0;
                    int tempFrameNameLength = tempFrameName.length();
                    do {
                        size_t foundOpen = tempFrameName.find_first_of("[", startStringSearchIndex + 1);
                        if (foundOpen != string::npos)
                        {
                            size_t foundClose = tempFrameName.find_first_of("]", foundOpen + 1);
                            if (foundClose != string::npos)
                            {
                                string frameClass = tempFrameName.substr(foundOpen + 1, foundClose - (foundOpen + 1));
                                //if (useLog) lg << "Extras: Current frame class: " << frameClass << " from " << tempFrameName << " + " << startStringSearchIndex << "\n";
                                // Check name match
                                for (list<string>::iterator it = classList.begin(); it != classList.end(); ++it)
                                {
                                    string className = (string)*it;
                                    if (frameClass.length() == className.length() && strcmp(&frameClass[0], &className[0]) == 0)
                                    {
                                        extraFramesMatchClass[classFrames] = tempFrame;
                                        //if (useLog) lg << "Extras: Added to match class: " << tempFrameName << "\n";
                                        classFrames++;
                                        break;
                                    }
                                }
                                startStringSearchIndex = foundClose;
                            }
                            else break;
                        }
                        else break;
                    } while ((tempFrameNameLength - 2) > startStringSearchIndex);
                }
                extraFrames[frames] = tempFrame;
                frames++;
            }
            else break;
            tempFrame = tempFrame->next;
        }


        // -- Randomize (:+) or just fix max
        if (randomizeVariations == true)
            selectVariations = Random(selectVariations, frames);
        else
            if (selectVariations > frames) selectVariations = frames;


        // -- Terminator for faster 'DeleteAllExtraFramesFromArray'
        extraFrames[(frames + 1)] = (RwFrame *)1;
        extraFramesMatchClass[(classFrames + 1)] = (RwFrame *)1;


        // -- Show has class
        //if (useLog) lg << "Extras: Childs found: " << frames << "\n";
        if (classFrames > 0) 
        {
           //if (useLog) lg << "Extras: Class childs found: " << classFrames << "\n";
        }


        // -- Num variations
        if (selectVariations == -1) 
        {
            if (frames == 1) selectVariations = 0;
            else selectVariations = 1;
        }
        //if (useLog) lg << "Extras: Select variations: " << selectVariations << "\n";



        // -- Select random extra(s)
        int i = 0;
        int random = 0;
        int loopClassFrames = classFrames;
        do {
            if (selectVariations == 0)  // if ":0" or : was not set and just 1 frame = 50% chance to not appear
            { 
                if (Random(0, frames) == 0) break;
            }
            if (loopClassFrames > 0)  // from class frames array
            { 
                random = Random(0, classFrames - 1);
                if (extraFramesMatchClass[random] != nullptr)
                {
                    i++;
                    //if (useLog) lg << "Extras: Starting new node (class match) " << i << " of " << selectVariations << " \n";
                    ProcessExtraRecursive(extraFramesMatchClass[random], vehicle);

                    RemoveFrameClassFromNormalArray(extraFramesMatchClass[random], extraFrames);
                    extraFramesMatchClass[random] = nullptr;

                    loopClassFrames--;
                }
            }
            else  // from normal frames array
            { 
                if (frames > 1) random = Random(0, frames - 1);
                if (extraFrames[random] != nullptr)
                {
                    if (FrameIsOtherClass(extraFrames[random]))
                    {
                        DestroyNodeHierarchyRecursive(extraFrames[random]);
                        extraFrames[random] = nullptr;
                        continue;
                    }
                    i++;
                   // if (useLog) lg << "Extras: Starting new node " << i << " of " << selectVariations << " \n";
                    ProcessExtraRecursive(extraFrames[random], vehicle);

                    extraFrames[random] = nullptr;
                }
                else
                {
                    int k = 0;
                    for (k = 0; k < frames; k++)
                    {
                        if (extraFrames[k] != nullptr) break;
                    }
                    if (k == frames)
                    {
                        //if (useLog) lg << "Extras: There is no more extras to select \n";
                        break;
                    }
                }
            }
        } while (i < selectVariations);


        // -- Delete all not selected nodes
        for (int i = 0; i < 33; i++) 
        {
            if (extraFrames[i] == (RwFrame *)1) break; // Terminator
            if (extraFrames[i] != nullptr && extraFrames[i] != 0) 
            {
                DestroyNodeHierarchyRecursive(extraFrames[i]);
            }
        }

    }
    //else if (useLog) lg << "Extras: Node has no child \n";

    return;
}

static void FindNodesRecursive(RwFrame * frame, CVehicle *vehicle, bool bReSearch, bool bOnExtras)
    {
        VehData &data = vehicleDataMap[vehicle];
        while (frame) 
        {
            const string name = GetFrameNodeName(frame);
            size_t found;
            
            if (name[0] == 'f' && name[1] == '_') 
            {
                if (!bReSearch) 
                {
                    // Don't process extras if seed is 0 (for Tuning Mod and other mods)
                    if (data.randomSeed != 0)
                    {
                        // Set extras class
                        found = name.find("f_class");
                        if (found != string::npos) 
                        {
                            //if (useLog) lg << "Extras: Found 'f_class' \n";

                            ProcessClassesRecursive(frame, vehicle, bReSearch, false);

                            if (RwFrame * tempFrame = frame->next) 
                            {
                                //if (useLog) lg << "Extras: Jumping class nodes \n";
                                frame = tempFrame;
                                continue;
                            }
                            else {
                                break;
                            }
                        }
                        
                        // Recursive extras
                        found = name.find("f_extras");
                        if (found != string::npos) 
                        {
                           // if (useLog) lg << "Extras: Found 'f_extras' \n";

                            RwFrame * tempFrame = frame->child;
                            if (tempFrame != nullptr) 
                            {
                                // Prepare for extra search
                                srand((data.randomSeed + data.randomSeedUsage));
                                data.randomSeedUsage++;

                                list<string> &classList = getClassList();

                               /* if (useLog) {
                                    lg << "Extras: Seed: " << xdata.randomSeed << "\n";
                                    lg << "Extras: Classes: ";
                                    for (list<string>::iterator it = classList.begin(); it != classList.end(); ++it) {
                                        lg << *it << " ";
                                    }
                                    lg << "\nExtras: --- Starting extras for veh ID " << vehicle->m_nModelIndex << "\n";
                                }*/

                                ProcessExtraRecursive(frame, vehicle);
                                //if (useLog) lg << "Extras: --- Ending \n";
                            }
                            //else if (useLog) lg << "Extras: (error) 'f_extras' has no childs \n";
                        }
                    }
				}
                
                // Shake
                found = name.find("f_shake");
                if (found != std::string::npos) {
                    if (CreateMatrixBackup(frame)) {
                        data.shakeFrame.push_back(frame);
                        FRAME_EXTENSION(frame)->owner = vehicle;
                    }
                }
                
                // Taxi light
                found = name.find("f_taxilight");
                if (found != string::npos)
                {
                    data.taxilightFrame = frame;
                    FRAME_EXTENSION(frame)->owner = vehicle;
				}
                 
               // Wheel
               found = name.find("f_wheel");
               if (found != string::npos) 
               {
                  for (int i = 0; i < 6; i++) 
                  {
                    if (!data.wheelFrame[i]) 
                    {
                      data.wheelFrame[i] = frame;
                      FRAME_EXTENSION(frame)->owner = vehicle;
                      break;
                    }
                  }
                }
                
              // Digital speedo
              found = name.find("f_dspeedo");
              if (found != string::npos) 
              {
                 SetupDigitalSpeedo(vehicle, frame);
                 data.speedoFrame = frame;
                 FRAME_EXTENSION(frame)->owner = vehicle;

                 float speedMult = 1.0;

                 found = name.find("_mph");
                 if (found != string::npos) 
                 {
                     speedMult *= 0.621371f;
                 }

                 found = name.find("_mu=");
                 if (found != string::npos) 
                 {
                     float mult = stof(&name[found + 4]);
                     speedMult *= mult;
                 }
        

                     data.speedoMult = speedMult;
	           }
             // Wiper
             found = name.find("f_wiper");
             if (found != std::string::npos) {
                 if (CreateMatrixBackup(frame)) {
                     F_an* an = new F_an(frame);
                     an->mode = 1001;
                     an->submode = 0;
                     data.anims.push_back(an);
                     FRAME_EXTENSION(frame)->owner = vehicle;
                 }
             }
             
             // Animation by condition
                found = name.find("f_an"); //f_an1a=
                if (found != string::npos)
                {
                    if (name[6] == '=')
                    {
                        int mode = stoi(&name[4]);
                        int submode = (name[5] - 'a');

                        switch (mode)
                        {
                        case 0:
                            ///if (useLog) lg << "Anims: Found 'f_an" << mode << "': ping pong \n";
                            break;
                        case 1:
                            switch (submode)
                            {
                            case 0:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": engine off \n";
                                break;
                            case 1:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": engine off or alarm on \n";
                                break;
                            default:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << " ERROR: submode not found \n";
                                submode = -1;
                                break;
                            }
                            break;
                        case 2:
                            switch (submode)
                            {
                            case 0:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": driver \n";
                                break;
                            case 1:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": passenger 1 \n";
                                break;
                            case 2:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": passenger 2 \n";
                                break;
                            case 3:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": passenger 3 \n";
                                break;
                            default:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << " ERROR: submode not found \n";
                                submode = -1;
                                break;
                            }
                            break;
                        case 3:
                            switch (submode)
                            {
                            case 0:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed \n";
                                break;
                            case 1:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed and f_spoiler \n";
                                data.spoilerFrames.push_back(frame);
                                break;
                            default:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << " ERROR: submode not found \n";
                                submode = -1;
                                break;
                            }
                            break;
                        case 4:
                            switch (submode)
                            {
                            case 0:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": brake \n";
                                break;
                            case 1:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed brake \n";
                                break;
                            case 2:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << "' " << submode << ": high speed brake and f_spoiler \n";
                                data.spoilerFrames.push_back(frame);
                                break;
                            default:
                                //if (useLog) lg << "Anims: Found 'f_an" << mode << " ERROR: submode not found \n";
                                submode = -1;
                                break;
                            }
                            break;
                        default:
                            //if (useLog) lg << "Anims: Found 'f_an' ERROR: mode not found \n";
                            mode = -1;
                            break;
                        }

                        if (mode >= 0 && submode >= 0)
                        {
                            if (CreateMatrixBackup(frame))
                            {
                                F_an *an = new F_an(frame);
                                an->mode = mode;
                                an->submode = submode;

                                data.anims.push_back(an);
                                FRAME_EXTENSION(frame)->owner = vehicle;
                            }
                        }
                    }
				}
             
             // Gaspedal
    found = name.find("f_gas");
    if (found != string::npos) 
    {
       if (CreateMatrixBackup(frame)) {
           data.gaspedalFrame.push_back(frame);
           FRAME_EXTENSION(frame)->owner = vehicle;
       }
    }

    // brakepedal
    found = name.find("f_brake");
    if (found != string::npos) 
    {
      if (CreateMatrixBackup(frame)) {
      data.brakepedalFrame.push_back(frame);
      FRAME_EXTENSION(frame)->owner = vehicle;
      }
    }
    
    // Gear
    if (vehicle->m_nVehicleSubClass == VEHICLE_PLANE || vehicle->m_nVehicleSubClass == VEHICLE_HELI)
    {
        found = name.find("f_spin");
    }
    else
    {
        found = name.find("f_gear");
    }
    if (found != string::npos) 
    {
        data.gearFrame.push_back(frame);
        FRAME_EXTENSION(frame)->owner = vehicle;
    }

    // Fan
    found = name.find("f_fan");
    if (found != string::npos) 
    {
       data.fanFrame.push_back(frame);
       FRAME_EXTENSION(frame)->owner = vehicle;
    }
    
    if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX || vehicle->m_nVehicleSubClass == VEHICLE_QUAD)
    {
       // footpeg driver
       found = name.find("f_fpeg1");
       if (found != string::npos) 
       {
          if (CreateMatrixBackup(frame)) {
              data.fpegFront.push_back(new F_footpegs(frame));
              FRAME_EXTENSION(frame)->owner = vehicle;
          }
       }

       // footpeg passenger
       found = name.find("f_fpeg2");
       if (found != string::npos) 
       {
         if (CreateMatrixBackup(frame)) {
            data.fpegBack.push_back(new F_footpegs(frame));
            FRAME_EXTENSION(frame)->owner = vehicle;
         }
       }
     }
     
     // tricycle fork
     found = name.find("f_trifork");
     if (found != string::npos)
     {
       if (CreateMatrixBackup(frame))
       {
          data.triforkFrame = frame;
          FRAME_EXTENSION(frame)->owner = vehicle;

          //if (!frame->child)
          //{
              RwFrame *wheelFrame = CClumpModelInfo::GetFrameFromId(vehicle->m_pRwClump, 5);
              RwFrameAddChild(frame->child, wheelFrame->child);
          //}
        }
	  }
      
      // Popup lights //f_popl=ax45s4
                found = name.find("f_pop");
                if (found != string::npos)
                {
                    if (CreateMatrixBackup(frame)) {
                        if (name[found + 5] == 'l') {
                            SetFrameNodeName(frame, "f_popl=ax45s1");
                            data.popupFrame[0] = frame;
                        }
                        else
                        {
                            SetFrameNodeName(frame, "f_popr=ax45s1");
                            data.popupFrame[1] = frame;
                        }
                        FRAME_EXTENSION(frame)->owner = vehicle;
                    }
				}
      
      // Spoiler
      found = name.find("f_spoiler");
                if (found != string::npos)
                {
                    data.spoilerFrames.push_back(frame);
                    FRAME_EXTENSION(frame)->owner = vehicle;
				}
                
            }//f_  end
            
            // Steer
                found = name.find("f_steer");
                if (found != string::npos)
                {
                   if (CreateMatrixBackup(frame)) 
                   {
                       data.steer.push_back(frame);
                       FRAME_EXTENSION(frame)->owner = vehicle;
                   }
                }
                
                found = name.find("movsteer");
                if (found != string::npos)
                {
                   found = name.find("movsteer");
                   if (found != string::npos)
                   {
                       if (name[8] == '_') 
                       {
                           if (isdigit(name[9]))
                           {
                               if (CreateMatrixBackup(frame)) 
                               {
                                  data.steer.push_back(frame);
                                  FRAME_EXTENSION(frame)->owner = vehicle;
                               }
                           }
                       }
                       else 
                       {
                         if (CreateMatrixBackup(frame)) 
                         {
                             data.steer.push_back(frame);
                             FRAME_EXTENSION(frame)->owner = vehicle;
                         }
                       }
                    }
                  }
    
                if (!APPinstalled) 
                {
                   found = name.find("steering_dummy");
                   if (found != string::npos)
                   {
                      if (frame->child)
                      {
                        if (CreateMatrixBackup(frame->child)) 
                        {
                           data.steer.push_back(frame->child);
                           FRAME_EXTENSION(frame->child)->owner = vehicle;
                        }
                      }
                    }
                 }
            
            // Wiper
             found = name.find("dvorleft");
             if (found != std::string::npos) {
                 if (CreateMatrixBackup(frame)) {
                     SetFrameNodeName(frame, "f_wiper=ay-60");
                     F_an* an = new F_an(frame);
                     an->mode = 1001;
                     an->submode = 0;
                     data.anims.push_back(an);
                     FRAME_EXTENSION(frame)->owner = vehicle;
                 }
             }
             
             // Wiper
             found = name.find("dvorright");
             if (found != std::string::npos) {
                 if (CreateMatrixBackup(frame)) {
                     SetFrameNodeName(frame, "f_wiper=ay-60");
                     F_an* an = new F_an(frame);
                     an->mode = 1001;
                     an->submode = 0;
                     data.anims.push_back(an);
                     FRAME_EXTENSION(frame)->owner = vehicle;
                 }
             }
             
             //HandleBar
             /*forks_front*/
             found = name.find("forks_front");
             if(found != string::npos)
             {
                if(CreateMatrixBackup(frame))
                {
                    data.m_pSource = frame;
                    FRAME_EXTENSION(frame)->owner = vehicle;
                }
             }
             
             /*handlebars*/
             found = name.find("handlebars");
             if(found != string::npos)
             {
                if(CreateMatrixBackup(frame))
                {
                    data.handleBarFrame = frame;
                    FRAME_EXTENSION(frame)->owner = vehicle;
                }
             }
             
             //TachoMeter
            found = name.find("tahook");
            if(found != string::npos)
            {
                if (CreateMatrixBackup(frame))
                {
                    data.tachoFrame = frame;
                    FRAME_EXTENSION(frame)->owner = vehicle;
                }
            }

            
            //SpeedMeter
            found = name.find("speedook");
            if(found != string::npos)
            {
                if (CreateMatrixBackup(frame))
                {
                    data.speedFrame = frame;
                    FRAME_EXTENSION(frame)->owner = vehicle;
                }
            }
            
            found = name.find("fc_sm");
            if(found != string::npos)
            {
                if (CreateMatrixBackup(frame))
                {
                    data.speedFrame = frame;
                    FRAME_EXTENSION(frame)->owner = vehicle;
                }
            }
            
            found = name.find("fc_rpm");
            if(found != string::npos)
            {
                if (CreateMatrixBackup(frame))
                {
                    data.rpmFrame = frame;
                    FRAME_EXTENSION(frame)->owner = vehicle;
                }
            } 
            
            // Shake
                found = name.find("hub_");
                if (found != std::string::npos) {
                    if (CreateMatrixBackup(frame)) {
                        data.wheelhubFrame.push_back(frame);
                        FRAME_EXTENSION(frame)->owner = vehicle;
                    }
                }
            
            // Characteristics
			//FindVehicleCharacteristicsFromNode(frame, vehicle, bReSearch);
            
            /////////////////////////////////////////
            if (RwFrame * newFrame = frame->child)  FindNodesRecursive(newFrame, vehicle, bReSearch, bOnExtras);
            if (RwFrame * newFrame = frame->next)   FindNodesRecursive(newFrame, vehicle, bReSearch, bOnExtras);
            return;
        }
        return;
    }

DECL_HOOKv(AddUpgrade, CVehicle *vehicle, int up)
{
    AddUpgrade(vehicle, up);
    VehData &data = vehicleDataMap[vehicle];
    data.flags.bUpgradesUpdated = true;
};

DECL_HOOKv(vehicleModelSet, CVehicle *vehicle, uint32_t modelId)
{
    vehicleModelSet(vehicle, modelId);
    VehData &data = vehicleDataMap[vehicle];
    data.nodesProcess = true;
    FeatureMgr::Add(static_cast<void*>(vehicle), (RwFrame *)vehicle->m_pRwClump->object.parent, eModelEntityType::Vehicle);
    VehicleMaterials::OnModelSet(vehicle, modelId);
};

bool TrailerLightsInstalled = false;
bool TrailersInstalled = false;

DECL_HOOKv(VehiclePreRender, CVehicle *vehicle)
{
    bool bReSearch = false;
    VehiclePreRender(vehicle);
}

extern list<CustomSeed> &getCustomSeedList()
{
    static list<CustomSeed> customSeedList;
    return customSeedList;
}

DECL_HOOKv(VehicleRender, CVehicle *vehicle)
        {
            VehData &data = vehicleDataMap[vehicle];
            
            // Init
            bool bReSearch = false;

            // Set custom seed
            list<CustomSeed> &customSeedList = getCustomSeedList();
            if (customSeedList.size() > 0)
            {
                list<CustomSeed> customSeedsToRemove;
               // if (useLog && bNewFrame) lg << "Custom Seed: Running list size " << customSeedList.size() << "\n";

                for (list<CustomSeed>::iterator it = customSeedList.begin(); it != customSeedList.end(); ++it)
                {
                    CustomSeed customSeed = *it;

                    if (reinterpret_cast<int>(vehicle) == customSeed.pvehicle)
                    {
                       // if (useLog) lg << "Custom Seed: Seed " << customSeed.seed << " set to " << customSeed.pvehicle << "\n";
                        data.randomSeed = customSeed.seed;
                        customSeedsToRemove.push_back(*it);
                    }
                    else {
                        if (CTimer::m_snTimeInMilliseconds > customSeed.timeToDeleteOfNotFound)
                        {
                            //if (useLog) lg << "Custom Seed: Not found vehicle " << customSeed.pvehicle << ". Time limit.\n";
                            customSeedsToRemove.push_back(*it);
                        }
                    }
                }
                for (list<CustomSeed>::iterator it = customSeedsToRemove.begin(); it != customSeedsToRemove.end(); ++it)
                {
                    customSeedList.remove(*it);
                }
                customSeedsToRemove.clear(); 
			}
            
            // Search nodes
            if (data.nodesProcess || bReSearch)
            {
                
                // Clear temp class list
                list<string> &classList = getClassList();
				classList.clear();
                
                // Process all nodes
                data.randomSeedUsage = 0;
                RwFrame *rootFrame = (RwFrame *)vehicle->m_pRwClump->object.parent;
                FindNodesRecursive(rootFrame, vehicle, bReSearch, false);
                
                if (!bReSearch)
                {
                    // Set wheels
                    if (data.wheelFrame[0]) SetWheel(data.wheelFrame, vehicle);

                    // Fix materials
                    FixMaterials(vehicle->m_pRwClump);
				}
                
                // Post set
				//SetCharacteristicsInRender(vehicle, bReSearch);
                data.nodesProcess = false;
            }
        
        // Process store smooth pedal
        int subClass = vehicle->m_nVehicleSubClass;
        int gasSoundProgress = vehicle->m_vehicleAudio.field_14C;
        int rpmSound = vehicle->m_vehicleAudio.field_148;

        if ((subClass == VEHICLE_AUTOMOBILE || subClass == VEHICLE_BIKE || subClass == VEHICLE_MTRUCK || subClass == VEHICLE_QUAD) &&
                gasSoundProgress == 0 && vehicle->m_fMovingSpeed > 0.2f && rpmSound != -1)
        { // todo: the last gear (max speed) is ignored, need to fix
                data.smoothGasPedal = 0.0f;
        }
        else
        {
            float gasPedal = abs(vehicle->m_fGasPedal);

            if (subClass == VEHICLE_PLANE || subClass == VEHICLE_HELI)
            {
                CVector vectorSpeed = vehicle->m_vecMoveSpeed;
                gasPedal = vectorSpeed.Magnitude() * 200.0;
                //Util::GetVehicleSpeedRealistic(vehicle) / 40.0f;
                if (gasPedal > 1.0f) gasPedal = 1.0f;
            }
                
            if (gasPedal > 0.0f)
            {
                data.smoothGasPedal += (CTimer::ms_fTimeStep / 1.6666f) * (gasPedal / 6.0f);
                if (data.smoothGasPedal > 1.0f) data.smoothGasPedal = 1.0f;
                else if (data.smoothGasPedal > gasPedal) data.smoothGasPedal = gasPedal;
            }
            else
            {
               if (data.smoothGasPedal > 0.0f)
               {
                  data.smoothGasPedal -= (CTimer::ms_fTimeStep / 1.6666f) * 0.3;
                  if (data.smoothGasPedal < 0.0f) data.smoothGasPedal = 0.0f;
               }
            }
          }
            
          float brakePedal = abs(vehicle->m_fBrakePedal);
          if (brakePedal > 0.0f)
          {
             data.smoothBrakePedal += (CTimer::ms_fTimeStep / 1.6666f) * (brakePedal / 6.0f);
             if (data.smoothBrakePedal > 1.0f) data.smoothBrakePedal = 1.0f;
             else if (data.smoothBrakePedal > brakePedal) data.smoothBrakePedal = brakePedal;
          }
          else
          {
             if (data.smoothBrakePedal > 0.0f)
             {
                 data.smoothBrakePedal -= (CTimer::ms_fTimeStep / 1.6666f) * 0.3;
                 if (data.smoothBrakePedal < 0.0f) data.smoothBrakePedal = 0.0f;
             }
          }
          
          // Process material stuff (before render)
            if (data.taxiSignMaterial)
            {
                if (reinterpret_cast<CAutomobile*>(vehicle)->m_nAutomobileFlags.bTaxiLight & 1)
                {
                    resetMats.push_back(std::make_pair(reinterpret_cast<unsigned int *>(&data.taxiSignMaterial->surfaceProps.ambient), *reinterpret_cast<unsigned int *>(&data.taxiSignMaterial->surfaceProps.ambient)));
                    data.taxiSignMaterial->surfaceProps.ambient = 10.0f;
                }
            }
            if (data.brakeDiscMaterial)
            {
                if (data.smoothBrakePedal > 0.0f) {
                    data.brakeHeat += data.smoothBrakePedal * (Util::GetVehicleSpeedRealistic(vehicle) / 8000.0f) * CTimer::ms_fTimeStep * 1.666667f;
                }
                if (data.brakeHeat > 0.0f)
                {
                    data.brakeHeat -= 0.004f * CTimer::ms_fTimeStep * 1.666667f;
                    if (data.brakeHeat < 0.0f) {
                        data.brakeHeat = 0.0f;
                    }
                    else {
                        if (data.brakeHeat > 1.0f) {
                            data.brakeHeat = 1.0f;
                        }
                        resetMats.push_back(std::make_pair(reinterpret_cast<unsigned int*>(&data.brakeDiscMaterial->surfaceProps.ambient), *reinterpret_cast<unsigned int*>(&data.brakeDiscMaterial->surfaceProps.ambient)));
                        resetMats.push_back(std::make_pair(reinterpret_cast<unsigned int*>(&data.brakeDiscMaterial->color.blue), *reinterpret_cast<unsigned int*>(&data.brakeDiscMaterial->color.blue)));
                        resetMats.push_back(std::make_pair(reinterpret_cast<unsigned int*>(&data.brakeDiscMaterial->color.green), *reinterpret_cast<unsigned int*>(&data.brakeDiscMaterial->color.green)));
                        if (data.brakeHeat > 0.1f) {
                            data.brakeDiscMaterial->surfaceProps.ambient = data.brakeDiscMaterial->surfaceProps.ambient + (data.brakeHeat - 0.1f);
                        }
                        data.brakeDiscMaterial->color.green = (int)(255.0f - (data.brakeHeat * (255.0f - (data.brakeHeat * 128.0f))));
                        data.brakeDiscMaterial->color.blue = (int)(255.0f - (data.brakeHeat * 255.0f));
                    }
                }
			}
            
        // Process steer
        if (!data.steer.empty()) ProcessSteer(vehicle, data.steer);
        
        // Process shake
        if (!data.shakeFrame.empty()) ProcessShake(vehicle, data.shakeFrame);
        
        if (!data.wheelhubFrame.empty()) ProcessHubs(vehicle, data.wheelhubFrame);
        // Process gas pedal
        if (!data.gaspedalFrame.empty()) ProcessPedal(vehicle, data.gaspedalFrame, 1);
        // Process brake pedal
        if (!data.brakepedalFrame.empty()) ProcessPedal(vehicle, data.brakepedalFrame, 2);
        // Process gear
        if (!data.gearFrame.empty()) ProcessRotatePart(vehicle, data.gearFrame, true);
        // Process fans
        if (!data.fanFrame.empty()) ProcessRotatePart(vehicle, data.fanFrame, false);
        // Process trifork
        if (data.triforkFrame) ProcessTrifork(vehicle, data.triforkFrame);
        if (vehicle->m_fHealth > 0 && !vehicle->m_nVehicleFlags.bEngineBroken && !vehicle->m_nVehicleFlags.bIsDrowning)
        {
           // Process anims
           if (!data.anims.empty()) ProcessAnims(vehicle, data.anims);
           //if(!data.wiperFrame.empty()) ProcessWiper(vehicle, data.wiperFrame);
           // Process popup lights
           if (data.popupFrame[0]) ProcessPopup(vehicle);
        }
        
        if(data.odoFrame != nullptr)
        {
            ProcessOdoMeter(vehicle, data.odoFrame);
        }
        
        //Rpm
        if(data.rpmFrame != nullptr)
        {
            ProcessRpmMeter(vehicle, data.rpmFrame);
        }
        
        //ProcessSpeedmeter
        if(data.speedFrame != nullptr)
        {
            ProcessSpeedMeter(vehicle, data.speedFrame);
        }
        
        //ProcesTacho
        if(data.tachoFrame != nullptr)
        {
            ProcessTachoMeter(vehicle, data.tachoFrame);
        }
        
        // Process speedo
        if (data.speedoFrame != nullptr)
        {
           if (data.speedoDigits != nullptr)
           {
              ProcessDigitalSpeedo(vehicle, data.speedoFrame);
           }
        }
        
        // Process tuning spoiler (before render)
        if (data.flags.bUpgradesUpdated)
        {
           ProcessSpoiler(vehicle, data.spoilerFrames, true);
        }
        
        // Process footpegs
        if (vehicle->m_nVehicleSubClass == VEHICLE_BIKE || vehicle->m_nVehicleSubClass == VEHICLE_BMX)
        {
           if (!data.fpegFront.empty()) ProcessFootpegs(vehicle, data.fpegFront, 1);
           if (!data.fpegBack.empty()) ProcessFootpegs(vehicle, data.fpegBack, 2);
        }
        
        if(cfg->Bind("TrailerLights", true, "Features"))
        {
            if(!TrailerLightsInstalled)
            {
                ProcessTrailerLights(vehicle);
            }
        }
        
        if(cfg->Bind("AutoPilot", true, "Features")->GetBool())
        {
            ProcessAutoPilot();
        }
        
        if(cfg->Bind("ReverseSxf", true, "Features")->GetBool())
        {
            ProcessReverseSfx();
        }
        
        /*if(cfg->Bind("TruckTrailers", true, "Features"))
        {
            if(!TrailersInstalled)
            {
                ProcessTrailer(vehicle);
            }
        }*/
        
        FeatureMgr::Process(static_cast<void*>(vehicle), eModelEntityType::Vehicle);
        VehicleMaterials::RestoreMaterials();
        VehicleMaterials::OnRender(vehicle);
        VehicleRender(vehicle);
};

void FeatureMgr::Initialize()
{
    m_FunctionTable["forks_front"] = HandleBar::AddSource;
    m_FunctionTable["handlebars"] = HandleBar::Process;
    
    Lights::Initialize();
}

DECL_HOOKv(PluginAttach)
{
    FramePluginOffset = RwFrameRegisterPlugin(sizeof(FramePlugin), PLUGIN_ID_STR, (RwPluginObjectConstructor)FramePlugin::Init, (RwPluginObjectDestructor)FramePlugin::Destroy, (RwPluginObjectCopy)FramePlugin::Copy);;
    PluginAttach();
};

DECL_HOOKv(GameProcess, char const* name)
{
    GameProcess(name);
    FeatureMgr::Initialize();
};

initRwEvent = []
{
    InitTexture();
    if (!settingsLoaded) {
        ReadSettingsFile();
        settingsLoaded = true;
    }
};

ConfigEntry *author;

#include "GTASA/CFont.h"

void OpenLink(const char* link)
{
    Call::Function<void, const char*>(libs.GetSym("_Z18OS_ServiceOpenLinkPKc"), link);
}

const char* linkT = "https://github.com/Pandagaming15/AdditionalVehicleFunction.git";

void Link()
{
    CFont::SetOrientation(ALIGN_CENTER);
    CFont::SetColor(CRGBA(70,70,255,255));
    CFont::SetDropShadowPosition(1);
    CFont::SetBackground(false, false);
    CFont::SetWrapx(500.0);
    CFont::SetScale(1.0, 1.5);
    CFont::SetFontStyle(FONT_SUBTITLES);
    CFont::SetProportional(true);
    char tPos[0xFF];
    char text[0xFF];
    sprintf(tPos, "Link: |%s|", linkT);
    //sprintf(tPos, "Link: \033[4m%s\033[0m", linkT);

    AsciiToGxtChar(tPos, text);
    
    CVector2D screenPos = GetScreenTopCenter();
    
    CFont::PrintString(screenPos.x, screenPos.y, text);
    
    if(IsTouched(screenPos.x, screenPos.y, 100.0f, 30))
    {
        OpenLink(linkT);
    }
}

void Message()
{
    CPed *player = FindPlayerPed();
    CFont::SetOrientation(ALIGN_CENTER);
    CFont::SetColor(CRGBA(255,255,255,255));
    CFont::SetDropShadowPosition(1);
    CFont::SetBackground(false, false);
    CFont::SetWrapx(500.0);
    CFont::SetScale(1.0, 1.5);
    CFont::SetFontStyle(FONT_SUBTITLES);
    CFont::SetProportional(true);
    char tPos[0xFF];
    char text[0xFF];
    sprintf(tPos, "%s By: %s | You are using a stolen mod! Please download the official one from the link above |", modinfo->Name(), modinfo->Author());
    AsciiToGxtChar(tPos, text);
    
    CVector2D screenPos = GetScreenCenter();
    
    CFont::PrintString(screenPos.x, screenPos.y, text);
}

drawHudEvent = []()
{
    Message();
    Link();
};

#include "shared/ini/inireader.h"

// Hookies

DECL_HOOKv(PauseOpenAL, void* self, int doPause)
{
    PauseOpenAL(self, doPause);
    doPause ? soundsys->PauseStreams() : soundsys->ResumeStreams();
}
DECL_HOOKv(GameShutdown)
{
    GameShutdown();
    soundsys->UnloadAllStreams();
}
DECL_HOOKv(GameShutdownEngine)
{
    GameShutdownEngine();
    soundsys->UnloadAllStreams();
}
DECL_HOOKv(GameRestart)
{
    GameRestart();
    soundsys->UnloadAllStreams();
}
DECL_HOOK(void*, UpdateGameLogic, uintptr_t a1)
{
    soundsys->Update();
    return UpdateGameLogic(a1);
}

extern "C" void OnModLoad()
{
    author = cfg->Bind("By", modinfo->Author(), "AVF");
    if(strcmp(author->GetString(), modinfo->Author()) == 0)
    {
        sautils = (ISAUtils*)GetInterface("SAUtils");
        void *trailerLights = aml->GetLibHandle("libTrailerLights.so");
        if(trailerLights)
        {
            TrailerLightsInstalled = true;
        }
        else
        {
            TrailerLightsInstalled = false;
        }
        
        void *trailers = aml->GetLibHandle("libTruckTrailer.so");
        if(trailers)
        {
            TrailersInstalled = true;
        }
        else
        {
            TrailersInstalled = false;
        }
    
        HOOK(VehicleRender, libs.GetSym("_ZN8CVehicle6RenderEv"));
        HOOKBLX(vehicleModelSet, libs.GetSym("_ZN8CVehicle13SetModelIndexEj") + 0x0C);
        HOOK(AddUpgrade, libs.GetSym("_ZN8CVehicle17AddVehicleUpgradeEi"));
        HOOKBLX(PluginAttach, libs.pGame + 0x3F6F66); // 0x3F6F66 _Z12PluginAttachv
        HOOK(GameProcess, libs.GetSym("_ZN5CGame5Init1EPKc"));
        PHookHandle pop = aml->HookInline((void*)(libs.pGame + 0x5911E0), HookPopUp);
        PHookHandle poplight = aml->HookInline((void*)(libs.pGame + 0x5911E4), HookPoplights);
        
        /*if(cfg->Bind("TruckTrailers", true, "Features")->GetBool())
        {
             //Trailers
            HOOKBLX(PlayAttachTrailerAudio, libs.GetSym("_ZN8CTrailer10SetTowLinkEP8CVehicleb") + 0xB6);
            void *CTrailer_BreakTowLink = aml->HookInline((void*)(libs.pGame + 0x57C026), BreakTowLink);
        }*/
        
        HOOKPLT(TouchEvent, libs.pGame + 0x675DE4);
        
        //autopilot
        SET_TO(gMobileMenu, libs.GetSym("gMobileMenu"));
        SET_TO(FrontEnd, libs.GetSym("FrontEndMenuManager"));
        SET_TO(ms_RadarTrace, libs.GetSym("_ZN6CRadar13ms_RadarTraceE"));
        
        SetEventBefore(initRw);
        SetEventBefore(drawing);
        
        //audiosystem
        SetEventBefore(processScripts);
        
    logger->SetTag("AudioStreams");
    
    if(!(BASS = (IBASS*)GetInterface("BASS")))
    {
        logger->Error("Cannot load: BASS interface is not found!");
        return;
    }
    
    SET_TO(camera, libs.GetSym("TheCamera"));
    SET_TO(userPaused, libs.GetSym("_ZN6CTimer11m_UserPauseE"));
    SET_TO(codePaused, libs.GetSym("_ZN6CTimer11m_CodePauseE"));
    SET_TO(m_snTimeInMillisecondsNonClipped, libs.GetSym("_ZN6CTimer32m_snTimeInMillisecondsNonClippedE"));
    SET_TO(m_snPreviousTimeInMillisecondsNonClipped, libs.GetSym("_ZN6CTimer40m_snPreviousTimeInMillisecondsNonClippedE"));
    SET_TO(ms_fTimeScale, libs.GetSym("_ZN6CTimer13ms_fTimeScaleE"));
    
    HOOKPLT(UpdateGameLogic, libs.pGame + 0x66FE58);
    HOOKPLT(PauseOpenAL, libs.pGame + 0x674BE0);
    HOOKPLT(GameShutdown, libs.pGame + 0x672864);
    HOOKPLT(GameShutdownEngine, libs.pGame + 0x6756F0);
    HOOKPLT(GameRestart, libs.pGame + 0x6731A0);

    SET_TO(AEAudioHardware, libs.GetSym("AEAudioHardware"));
    SET_TO(GetEffectsMasterScalingFactor, libs.GetSym("_ZN16CAEAudioHardware29GetEffectsMasterScalingFactorEv"));
    SET_TO(GetMusicMasterScalingFactor, libs.GetSym("_ZN16CAEAudioHardware27GetMusicMasterScalingFactorEv"));
    
    SET_TO(GetObjectFromRef, libs.GetSym("_ZN6CPools9GetObjectEi"));
    SET_TO(GetPedFromRef, libs.GetSym("_ZN6CPools6GetPedEi"));
    SET_TO(GetVehicleFromRef, libs.GetSym("_ZN6CPools10GetVehicleEi"));
    SET_TO(Get_Just_Switched_Status, libs.GetSym("_ZN7CCamera24Get_Just_Switched_StatusEv"));
    
    soundsys->Init();
    }
    else
    {
        SetEventBefore(drawHud);
    }
}
