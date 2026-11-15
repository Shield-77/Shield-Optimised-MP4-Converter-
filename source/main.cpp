#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <cstring>
#include <string>
#include <vector>
#include "shield_language_choice.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#define H264E_MAX_THREADS 2
#include "minih264e.h"
#include "minimp4.h"
u32 __stacksize__ = 256 * 1024;
}

static const char *OUT_DIR="sdmc:/SHIELD_MP4";
static PrintConsole lower;
static C3D_RenderTarget *topTarget=nullptr;
static C2D_TextBuf uiText=nullptr;
static bool cancelled=false;
static unsigned converted=0, skipped=0, declined=0, failed=0;
static uint64_t lastDraw=0;
static unsigned progressPercent=0;
static int currentDurationSeconds=0;
static int conversionFps=24;
static uint64_t sourceBytes=0;
static uint64_t conversionStarted=0;
static bool encoderUseSecondCore=false;
static bool wifiRestoreOnExit=false;
static bool cpuLimitChanged=false;
static u32 previousCpuLimit=0;
struct BrowserEntry { std::string name, path; bool directory; };
struct BrowserSnapshot { std::string path; std::vector<BrowserEntry> entries; size_t selected; };
static std::string currentDir="sdmc:/";
static std::vector<BrowserEntry> entries;
static std::vector<BrowserSnapshot> browserHistory;
static std::map<std::string,bool> folderHasMp4;
struct MediaInfo { bool eligible=false; int width=0, height=0; int64_t seconds=0; uint64_t bytes=0; };
static std::map<std::string,MediaInfo> mediaCache;
static size_t selected=0;
static bool browserReadError=false;
static size_t scanVisited=0;
static std::string lastReason;
static ShieldLanguageChoice uiLanguage={false,false,nullptr};

class HomeActionGuard {
public:
    HomeActionGuard() { aptSetHomeAllowed(false); }
    ~HomeActionGuard() { aptSetHomeAllowed(true); }
    HomeActionGuard(const HomeActionGuard &) = delete;
    HomeActionGuard &operator=(const HomeActionGuard &) = delete;
};

static const char *langText(const char *fr,const char *en) {
    return uiLanguage.french ? fr : en;
}
static void top(const char *heading,const char *line1,const char *line2);

static bool guideWasSeen() {
    FILE *f=fopen("sdmc:/3ds/ShieldMP4Converter/guide_seen.dat","rb");
    if(!f) return false;
    fclose(f);
    return true;
}
static void rememberGuide() {
    FILE *f=fopen("sdmc:/3ds/ShieldMP4Converter/guide_seen.dat","wb");
    if(f) { fputc(1,f); fclose(f); }
}
static void drawHelpPage(unsigned page) {
    const char *title[3]={
        langText("CHOISIR UN MP4","CHOOSE AN MP4"),
        langText("ANALYSE ET COMPATIBILITE","SCAN AND COMPATIBILITY"),
        langText("CONVERSION ET SORTIE","CONVERSION AND OUTPUT")
    };
    const char *line[3][4]={
        {langText("A ouvre un dossier ou lance un fichier.","A opens a folder or starts a file."),
         langText("Y analyse la carte SD complete.","Y scans the whole SD card."),
         langText("B remonte au dossier precedent.","B returns to the parent folder."),
         langText("START quitte l'application.","START quits the app.")},
        {langText("Seuls les MP4 H.264 avec AAC sont acceptes.","Only H.264 MP4 with AAC audio is supported."),
         langText("La sortie est limitee a 144p et 480p.","Output ranges from 144p up to 480p."),
         langText("Les doublons sont ignores automatiquement.","Duplicate files are skipped automatically."),
         langText("Une analyse longue affiche sa progression.","Long scans show their progress.")},
        {langText("15 fps : moins de charge; 24 fps : plus fluide.","15 fps: lighter; 24 fps: smoother."),
         langText("Les fichiers sont enregistres dans SHIELD_MP4.","Files are saved in SHIELD_MP4."),
         langText("Pas de lofi : moins de CPU et CIA plus leger.","No lofi: lower CPU use and a smaller CIA."),
         langText("START annule l'action en cours.","START cancels the current operation.")}
    };
    top(langText("GUIDE RAPIDE","QUICK GUIDE"),title[page],langText("A : continuer       B : retour","A: continue       B: back"));
    consoleSelect(&lower); consoleClear();
    printf("%s  %u/3\n\n",title[page],page+1);
    for(unsigned i=0;i<4;i++) printf("%s\n\n",line[page][i]);
    printf("%s\n",langText(page==2?"A : j'ai compris     B : retour":"A : suivant     B : retour",
                                    page==2?"A: got it     B: back":"A: next     B: back"));
}
static void drawLanguagePrompt() {
    top(langText("CHOIX DE LANGUE","LANGUAGE"),
        uiLanguage.french?"La langue detectee est le francais ?":"Is English your preferred language?",
        langText("A : confirmer   B : changer","A: confirm   B: switch"));
    consoleSelect(&lower); consoleClear();
    printf("%s\n\n",langText("Seules deux langues sont disponibles :","Only two languages are available:"));
    printf("%s\n\n",langText("Francais et English.","English and French."));
    printf("%s\n\n",langText("A : enregistrer le choix","A: save this choice"));
    printf("%s\n\n",langText("B : passer a l'autre langue","B: switch to the other language"));
    printf("%s\n",langText("L'anglais peut contenir des erreurs.","My English may contain mistakes."));
}
static bool chooseLanguage(bool forced) {
    const bool ready=R_SUCCEEDED(cfguInit());
    shield_language_choice_load(&uiLanguage,"sdmc:/3ds/ShieldMP4Converter/ui-language.dat",ready);
    if(ready) cfguExit();
    if(!forced && !uiLanguage.needs_confirmation) return true;
    drawLanguagePrompt();
    while(aptMainLoop()) {
        hidScanInput(); u32 keys=hidKeysDown();
        if(keys&KEY_START) return false;
        if(keys&KEY_B) { shield_language_choice_toggle(&uiLanguage); drawLanguagePrompt(); }
        if(keys&KEY_A) { shield_language_choice_accept(&uiLanguage); return true; }
        gspWaitForVBlank();
    }
    return false;
}
static bool showFirstRunGuide(bool review) {
    if(!review && guideWasSeen()) return true;
    unsigned page=0;
    drawHelpPage(page);
    while(aptMainLoop()) {
        hidScanInput(); u32 keys=hidKeysDown();
        if(keys&KEY_START) return false;
        bool changed=false;
        if(keys&KEY_B) { if(page) { page--; changed=true; } }
        if(keys&KEY_A) {
            if(page<2) { page++; changed=true; }
            else { rememberGuide(); return true; }
        }
        if(changed) drawHelpPage(page);
        gspWaitForVBlank();
    }
    return false;
}

struct H264Job { void (*run)(void *); void *data; };
static void runH264Job(void *opaque) {
    H264Job *job=(H264Job*)opaque;
    job->run(job->data);
}
static void runH264Jobs(void *, void (*jobFunc)(void *), void *jobData[], int jobCount) {
    if(jobCount<=0) return;
    if(jobCount>1 && encoderUseSecondCore) {
        H264Job job={jobFunc,jobData[1]};
        Thread worker=threadCreate(runH264Job,&job,48*1024,0x30,1,false);
        if(worker) {
            jobFunc(jobData[0]);
            if(R_SUCCEEDED(threadJoin(worker,U64_MAX))) threadFree(worker);
            for(int i=2;i<jobCount;i++) jobFunc(jobData[i]);
            return;
        }
    }
    for(int i=0;i<jobCount;i++) jobFunc(jobData[i]);
}

static void disableWifiForApp() {
    if(R_FAILED(acInit())) return;
    u32 status=0;
    Result statusResult=ACU_GetStatus(&status);
    acExit();
    // Only attempt the toggle when the AC service returned a known status.
    // Restore Wi-Fi only if this app successfully disabled it.
    if(R_FAILED(statusResult) || (status!=1 && status!=3)) return;
    wifiRestoreOnExit=R_SUCCEEDED(svcSetWifiEnabled(false));
}
static void restoreWifiAfterApp() {
    if(wifiRestoreOnExit) {
        svcSetWifiEnabled(true);
        wifiRestoreOnExit=false;
    }
}
static void beginConversionResources() {
    if(R_SUCCEEDED(APT_GetAppCpuTimeLimit(&previousCpuLimit)) &&
       R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) {
        cpuLimitChanged=true;
        encoderUseSecondCore=true;
    }
}
static void endConversionResources() {
    encoderUseSecondCore=false;
    if(cpuLimitChanged) {
        APT_SetAppCpuTimeLimit(previousCpuLimit);
        cpuLimitChanged=false;
    }
}
static const char *helpFor(const std::string &reason) {
    if(reason.find("SD")!=std::string::npos || reason.find("ecriture")!=std::string::npos || reason.find("Ecriture")!=std::string::npos)
        return "Verifiez la carte SD et son espace libre.";
    if(reason.find("Memoire")!=std::string::npos)
        return "Relancez l'app et choisissez un MP4 plus petit.";
    if(reason.find("Audio")!=std::string::npos)
        return "Ce MP4 doit contenir de l'AAC mono/stereo.";
    if(reason.find("Video")!=std::string::npos || reason.find("incompatible")!=std::string::npos)
        return "Choisissez un MP4 H.264 entre 145p et 480p.";
    if(reason.find("Finalisation")!=std::string::npos || reason.find("Renommage")!=std::string::npos)
        return "Liberez de l'espace sur la SD puis reessayez.";
    return "Reessayez avec un autre MP4 ou relancez l'app.";
}
static int readSdFile(void *opaque, uint8_t *buffer, int size) {
    FILE *f=(FILE*)opaque;
    size_t count=fread(buffer,1,size,f);
    return count ? (int)count : (feof(f) ? AVERROR_EOF : AVERROR(EIO));
}
static int64_t seekSdFile(void *opaque, int64_t offset, int whence) {
    FILE *f=(FILE*)opaque;
    if(whence&AVSEEK_SIZE) {
        off_t old=ftello(f); if(old<0 || fseeko(f,0,SEEK_END)!=0) return AVERROR(EIO);
        off_t size=ftello(f); fseeko(f,old,SEEK_SET); return size;
    }
    whence&=~AVSEEK_FORCE;
    if((int64_t)(off_t)offset!=offset || fseeko(f,(off_t)offset,whence)!=0) return AVERROR(EIO);
    return ftello(f);
}

static const char *translateTop(const char *s) {
    if(!s || uiLanguage.french) return s;
    struct Pair { const char *fr; const char *en; };
    static const Pair table[]={
        {"Analyse des MP4","Scanning MP4 files"},{"Explorateur SD","SD browser"},
        {"Recherche SD","SD scan"},{"Verification des doublons","Checking duplicates"},
        {"Conversion 256 x 144","Converting to 256 x 144"},{"Conversion longue","Long conversion"},
        {"Cadence de sortie","Output frame rate"},{"Aucun MP4 compatible","No compatible MP4"},
        {"Carte SD inaccessible","SD card unavailable"},{"Conversion annulee","Conversion cancelled"},
        {"Conversion terminee","Conversion complete"},{"Lecture des informations...","Reading file information..."},
        {"Ouverture des dossiers...","Opening folders..."},{"A : fichier   Y : toute la SD","A: file   Y: scan all SD"},
        {"Choisir un fichier MP4","Choose an MP4 file"},{"A : fichier  |  Y : toute la SD","A: file  |  Y: scan all SD"},
        {"Recherche des MP4...","Searching for MP4 files..."},{"Patienter","Please wait"},
        {"Liste des MP4...","Listing MP4 files..."},{"START : annuler","START: cancel"},
        {"A : confirmer   B : ignorer","A: convert   B: skip"},
        {"A : 15 fps (charge reduite)","A: 15 fps (lower workload)"},
        {"X : 24 fps (plus fluide) | B : retour","X: 24 fps (smoother) | B: back"},
        {"H.264/AAC de plus de 144p","H.264/AAC above 144p"},{"480p maximum","480p maximum"},
        {"Dossier SHIELD_MP4 impossible","Cannot access SHIELD_MP4 folder"},
        {"Verifier l'espace libre et la SD","Check free space and the SD card"},
        {"Sortie : /SHIELD_MP4","Output: /SHIELD_MP4"}
    };
    for(const Pair &p:table) if(strcmp(s,p.fr)==0) return p.en;
    return s;
}
static void top(const char *heading, const char *line1, const char *line2) {
    if(!topTarget || !uiText) return;
    C2D_TextBufClear(uiText);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(topTarget,C2D_Color32(2,18,35,255));
    C2D_SceneBegin(topTarget);
    C2D_DrawRectSolid(0,0,0,400,7,C2D_Color32(13,211,234,255));
    C2D_DrawRectSolid(15,20,0,370,199,C2D_Color32(5,36,59,255));
    C2D_Text t;
    C2D_TextParse(&t,uiText,"SHIELD  /  144p CONVERTER"); C2D_TextOptimize(&t);
    C2D_DrawText(&t,C2D_WithColor,28,30,0,0.62f,0.62f,C2D_Color32(23,213,241,255));
    C2D_TextParse(&t,uiText,translateTop(heading)); C2D_TextOptimize(&t);
    C2D_DrawText(&t,C2D_WithColor,28,73,0,0.74f,0.74f,C2D_Color32(255,255,255,255));
    C2D_TextParse(&t,uiText,translateTop(line1)); C2D_TextOptimize(&t);
    C2D_DrawText(&t,C2D_WithColor,28,113,0,0.52f,0.52f,C2D_Color32(208,224,237,255));
    C2D_TextParse(&t,uiText,translateTop(line2)); C2D_TextOptimize(&t);
    C2D_DrawText(&t,C2D_WithColor,28,147,0,0.56f,0.56f,C2D_Color32(23,213,241,255));
    C2D_DrawRectSolid(28,190,0,344,9,C2D_Color32(22,59,79,255));
    if(progressPercent) C2D_DrawRectSolid(28,190,0,344*std::min(progressPercent,100u)/100.0f,9,C2D_Color32(23,213,241,255));
    C3D_FrameEnd(0);
}
static bool controls() {
    if(!aptMainLoop()) return false;
    hidScanInput();
    if(hidKeysDown()&KEY_START) cancelled=true;
    return !cancelled;
}
static bool mp4name(const std::string &name) {
    if(name.size()<4) return false;
    std::string ext=name.substr(name.size()-4);
    for(char &c:ext) if(c>='A'&&c<='Z') c=(char)(c+32);
    return ext==".mp4";
}
static std::string joinPath(const std::string &dir,const std::string &name) {
    return dir=="sdmc:/" ? dir+name : dir+"/"+name;
}
static MediaInfo probeMp4(const std::string &path) {
    auto known=mediaCache.find(path);
    if(known!=mediaCache.end()) return known->second;
    static uint64_t lastProbeDraw=0;
    uint64_t now=osGetTime();
    if(!lastProbeDraw || now-lastProbeDraw>=500) {
        lastProbeDraw=now;
        progressPercent=0;
        top("Analyse des MP4",path.substr(0,38).c_str(),"Lecture des informations...");
    }
    MediaInfo info;
    struct stat st;
    if(stat(path.c_str(),&st)==0 && st.st_size>0) info.bytes=(uint64_t)st.st_size;
    AVFormatContext *fmt=avformat_alloc_context();
    AVIOContext *io=nullptr;
    FILE *file=nullptr;
    if(fmt) {
        fmt->probesize=128*1024;
        fmt->max_analyze_duration=AV_TIME_BASE/2;
        std::string url="file:"+path;
        if(avformat_open_input(&fmt,url.c_str(),nullptr,nullptr)<0) {
            if(fmt) avformat_close_input(&fmt);
            file=fopen(path.c_str(),"rb");
            uint8_t *buffer=file?(uint8_t*)av_malloc(32768):nullptr;
            if(buffer) io=avio_alloc_context(buffer,32768,0,file,readSdFile,nullptr,seekSdFile);
            if(!io) av_free(buffer);
            fmt=io?avformat_alloc_context():nullptr;
            if(fmt) {
                fmt->pb=io; fmt->flags|=AVFMT_FLAG_CUSTOM_IO;
                fmt->probesize=128*1024;
                fmt->max_analyze_duration=AV_TIME_BASE/2;
                if(avformat_open_input(&fmt,"local.mp4",nullptr,nullptr)<0) avformat_close_input(&fmt);
            }
        }
    }
    if(fmt) {
        bool audioOK=true,videoFound=false,audioFound=false;
        for(unsigned i=0;i<fmt->nb_streams;i++) {
            const AVCodecParameters *p=fmt->streams[i]->codecpar;
            if(!videoFound && p->codec_type==AVMEDIA_TYPE_VIDEO) {
                videoFound=true; info.width=p->width; info.height=p->height;
                info.eligible=p->codec_id==AV_CODEC_ID_H264 && p->width>=16 && p->width<=854 && p->height>144 && p->height<=480;
            } else if(!audioFound && p->codec_type==AVMEDIA_TYPE_AUDIO) {
                audioFound=true;
                audioOK=p->codec_id==AV_CODEC_ID_AAC && p->sample_rate>0 && p->sample_rate<=48000 &&
                        p->channels>=1 && p->channels<=2 && p->extradata_size>=2;
            }
        }
        info.eligible=info.eligible && audioOK;
        if(fmt->duration>0) info.seconds=fmt->duration/AV_TIME_BASE;
        avformat_close_input(&fmt);
    }
    if(io) { av_freep(&io->buffer); avio_context_free(&io); }
    if(file) fclose(file);
    if(mediaCache.size()>=4096) mediaCache.clear();
    mediaCache[path]=info;
    return info;
}
static bool containsMp4(const std::string &dir,unsigned depth=0) {
    auto known=folderHasMp4.find(dir);
    if(known!=folderHasMp4.end()) return known->second;
    if(depth>=128 || dir.size()>=1024) return false;
    DIR *d=opendir(dir.c_str()); if(!d) return false;
    std::vector<std::string> subdirs;
    bool found=false;
    struct dirent *e;
    while((e=readdir(d))) {
        if((++scanVisited&63)==0 && !controls()) break;
        std::string name=e->d_name;
        if(name.empty() || name=="." || name==".." || name[0]=='.') continue;
        std::string full=joinPath(dir,name);
        if(full==OUT_DIR) continue;
        bool directory=e->d_type==DT_DIR;
        if(e->d_type!=DT_DIR && e->d_type!=DT_REG) {
            struct stat st;
            if(stat(full.c_str(),&st)!=0) continue;
            directory=S_ISDIR(st.st_mode);
        }
        if(directory) subdirs.push_back(full);
        else if(mp4name(name) && probeMp4(full).eligible) { found=true; break; }
    }
    closedir(d);
    for(const std::string &subdir:subdirs) {
        if(found || cancelled) break;
        found=containsMp4(subdir,depth+1);
    }
    if(folderHasMp4.size()>=512) folderHasMp4.clear();
    folderHasMp4[dir]=found;
    return found;
}
static bool loadCurrentDirectory() {
    HomeActionGuard homeGuard;
    entries.clear(); selected=0; browserReadError=false;
    progressPercent=0;
    scanVisited=0;
    DIR *d=opendir(currentDir.c_str());
    if(!d) { browserReadError=true; return false; }
    std::vector<BrowserEntry> folders;
    struct dirent *e;
    while((e=readdir(d))) {
        std::string n=e->d_name;
        if(n.empty() || n=="." || n==".." || n[0]=='.') continue;
        std::string full=joinPath(currentDir,n);
        if(full==OUT_DIR) continue;
        bool directory=e->d_type==DT_DIR;
        if(e->d_type!=DT_DIR && e->d_type!=DT_REG) {
            struct stat st;
            if(stat(full.c_str(),&st)!=0) continue;
            directory=S_ISDIR(st.st_mode);
        }
        if(directory) folders.push_back({n,full,true});
        else if(mp4name(n) && probeMp4(full).eligible) entries.push_back({n,full,false});
        if((++scanVisited&15)==0 && !controls()) break;
    }
    closedir(d);
    for(size_t i=0;i<folders.size() && !cancelled;i++) {
        if(containsMp4(folders[i].path)) entries.push_back(folders[i]);
        if((i&7)==0) {
            char note[80]; snprintf(note,sizeof(note),"%u / %u dossiers",(unsigned)(i+1),(unsigned)folders.size());
            top("Explorateur SD",currentDir.c_str(),note);
        }
    }
    std::sort(entries.begin(),entries.end(),[](const BrowserEntry &a,const BrowserEntry &b) {
        if(a.directory!=b.directory) return a.directory>b.directory;
        return a.name<b.name;
    });
    return true;
}
static std::string shortName(const std::string &name,size_t limit) {
    if(name.size()<=limit) return name;
    size_t cut=limit;
    while(cut>0 && ((unsigned char)name[cut]&0xC0)==0x80) --cut;
    return name.substr(0,cut)+"...";
}
static void showBrowser() {
    consoleSelect(&lower); consoleClear();
    printf("\x1b[36mSHIELD 144p CONVERTER\x1b[0m\n");
    std::string location=shortName(currentDir.size()>6?currentDir.substr(6):"/",26);
    printf("\x1b[33mDossier : %s\x1b[0m\n",location.c_str());
    printf("%u MP4 a convertir / dossiers\n\n",(unsigned)entries.size());
    if(browserReadError) printf("\x1b[31mDossier inaccessible.\x1b[0m\n");
    else if(entries.empty()) printf("\x1b[90mAucun MP4 convertible ici.\nH.264/AAC, plus de 144p, 480p max.\x1b[0m\n");
    size_t first=(selected/16)*16;
    for(size_t i=first;i<entries.size() && i<first+16;i++) {
        const BrowserEntry &entry=entries[i];
        printf(i==selected?"\x1b[30;103m> ":"  ");
        printf(entry.directory?"\x1b[36m[DOSSIER] ":"\x1b[37m[MP4] ");
        std::string label=shortName(entry.name,24);
        printf("%s\x1b[0m\n",label.c_str());
    }
    printf("\n\x1b[32mA : ouvrir / convertir\x1b[0m  B : retour\nHaut/bas : choix | Gauche/droite : page\nY : toute la SD | START : quitter\n");
    if(failed && !lastReason.empty())
        printf("\n\x1b[31mProbleme : %.37s\x1b[0m\n%.38s\n",lastReason.c_str(),helpFor(lastReason));
}
static void collect(const std::string &dir, std::vector<std::string> &out, int depth) {
    if(depth>32 || out.size()>4096 || dir.size()>900 || cancelled) return;
    DIR *d=opendir(dir.c_str()); if(!d) return;
    struct dirent *e;
    while((e=readdir(d)) && !cancelled) {
        if((++scanVisited&63)==0) {
            char count[80]; snprintf(count,sizeof(count),"%u elements examines",(unsigned)scanVisited);
            top("Recherche SD",count,"START : annuler");
            if(!controls()) break;
        }
        std::string n=e->d_name;
        if(n.empty() || n=="." || n==".." || n[0]=='.') continue;
        std::string full=dir=="sdmc:/" ? dir+n : dir+"/"+n;
        if(full==OUT_DIR) continue;
        bool folder=e->d_type==DT_DIR;
        if(e->d_type!=DT_DIR && e->d_type!=DT_REG) {
            struct stat st; if(stat(full.c_str(),&st)!=0) continue;
            folder=S_ISDIR(st.st_mode);
        }
        if(folder) collect(full,out,depth+1);
        else if(mp4name(n) && probeMp4(full).eligible) out.push_back(full);
    }
    closedir(d);
}
static uint64_t fingerprint(const std::string &path, uint64_t &bytes) {
    FILE *f=fopen(path.c_str(),"rb"); if(!f) return 0;
    uint64_t total=0;
    if(fseeko(f,0,SEEK_END)==0) {
        off_t end=ftello(f);
        if(end>0) total=(uint64_t)end;
        fseeko(f,0,SEEK_SET);
    }
    uint64_t h=14695981039346656037ULL; bytes=0;
    unsigned char data[8192]; size_t n;
    while((n=fread(data,1,sizeof(data),f))!=0) {
        bytes+=n;
        for(size_t i=0;i<n;i++) { h^=data[i]; h*=1099511628211ULL; }
        if((bytes&0x1ffff)<sizeof(data)) {
            uint64_t now=osGetTime();
            if(now-lastDraw>=500) {
                lastDraw=now;
                unsigned pct=total ? (unsigned)std::min<uint64_t>(99,bytes*100/total) : 0;
                progressPercent=pct;
                char info[80];
                if(total) snprintf(info,sizeof(info),"Verification : %u%%",pct);
                else snprintf(info,sizeof(info),"Verification : %llu Mo",(unsigned long long)(bytes>>20));
                top("Verification des doublons",path.substr(0,38).c_str(),info);
                consoleSelect(&lower); consoleClear();
                printf("SHIELD / 144p CONVERTER\n\n%s\n\n%s\n\nSTART : annuler\n",path.substr(0,36).c_str(),info);
            }
            if(!controls()) { fclose(f); return 0; }
        }
    }
    bool ok=!ferror(f); fclose(f);
    h^=bytes; h*=1099511628211ULL;
    return ok ? h : 0;
}
static std::string basenameOf(const std::string &path) {
    size_t slash=path.find_last_of("/\\");
    return slash==std::string::npos ? path : path.substr(slash+1);
}
static std::string truncateUtf8(const std::string &text,size_t maxBytes) {
    if(text.size()<=maxBytes) return text;
    size_t cut=maxBytes;
    while(cut>0 && ((unsigned char)text[cut]&0xC0)==0x80) --cut;
    return text.substr(0,cut);
}
static bool outputContainsFingerprint(uint64_t hash,uint64_t bytes) {
    DIR *dir=opendir(OUT_DIR);
    if(!dir) return false;
    bool found=false;
    struct dirent *entry;
    while((entry=readdir(dir))) {
        std::string name=entry->d_name;
        if(name.size()<7 || name.compare(name.size()-7,7,".mp4.id")!=0) continue;
        std::string path=joinPath(OUT_DIR,name);
        FILE *file=fopen(path.c_str(),"rb");
        if(!file) continue;
        unsigned long long savedHash=0,savedBytes=0;
        bool valid=fscanf(file,"%llx %llx",&savedHash,&savedBytes)==2;
        fclose(file);
        if(valid && savedHash==(unsigned long long)hash && savedBytes==(unsigned long long)bytes) { found=true; break; }
    }
    closedir(dir);
    return found;
}
static bool writeFingerprint(const std::string &path,uint64_t hash,uint64_t bytes) {
    FILE *file=fopen(path.c_str(),"wb");
    if(!file) return false;
    bool ok=fprintf(file,"%016llx %llx\n",(unsigned long long)hash,(unsigned long long)bytes)>0;
    if(fflush(file)!=0 || ferror(file)) ok=false;
    if(fclose(file)!=0) ok=false;
    return ok;
}
static bool chooseOutputPaths(const std::string &source,std::string &final,std::string &temp,std::string &fingerprintPath) {
    std::string base=basenameOf(source);
    if(base.size()>=4) base.resize(base.size()-4);
    for(unsigned index=1;index<=10000;index++) {
        char numbered[24];
        if(index==1) snprintf(numbered,sizeof(numbered),"_optimised");
        else snprintf(numbered,sizeof(numbered),"_optimised_%u",index);
        std::string suffix=numbered;
        // Leave room for the longest temporary/metadata suffix within FAT's 255-byte component limit.
        std::string stem=truncateUtf8(base,247-suffix.size())+suffix;
        std::string name=stem+".mp4";
        final=joinPath(OUT_DIR,name);
        temp=final+".tmp";
        fingerprintPath=final+".id";
        struct stat st;
        if(stat(final.c_str(),&st)==0 || stat(temp.c_str(),&st)==0 || stat(fingerprintPath.c_str(),&st)==0) continue;
        return true;
    }
    return false;
}
static int writeMp4(int64_t offset, const void *buf, size_t count, void *opaque) {
    FILE *f=(FILE*)opaque;
    if(fseeko(f,(off_t)offset,SEEK_SET)!=0) return 1;
    return fwrite(buf,1,count,f)==count ? 0 : 1;
}
static void progress(const std::string &name, const char *stage, int frames, int64_t positionUs, int64_t bytePosition, bool force=false) {
    uint64_t now=osGetTime(); if(!force && now-lastDraw<500) return;
    lastDraw=now;
    char info[100], count[100];
    unsigned estimate=0;
    if(currentDurationSeconds>0 && positionUs>=0)
        estimate=(unsigned)std::min<int64_t>(99,positionUs*100/((int64_t)currentDurationSeconds*1000000));
    else if(sourceBytes && bytePosition>=0)
        estimate=(unsigned)std::min<uint64_t>(99,(uint64_t)bytePosition*100/sourceBytes);
    progressPercent=std::max(progressPercent,estimate);
    unsigned elapsed=conversionStarted ? (unsigned)((now-conversionStarted)/1000) : 0;
    snprintf(info,sizeof(info),"%u%% | %s | %u:%02u | %d images | max %d fps",progressPercent,stage,elapsed/60,elapsed%60,frames,conversionFps);
    snprintf(count,sizeof(count),"OK %u   Doublons %u   Erreurs %u",converted,skipped,failed);
    top("Conversion 256 x 144",name.substr(0,38).c_str(),info);
    consoleSelect(&lower); consoleClear();
    printf("\x1b[36mSHIELD / 144p CONVERTER\x1b[0m\n\n%s\n\n%s\n\nSTART : annuler\n",count,info);
}
static bool encodeYuvFrame(const std::string &source, H264E_persist_t *enc, H264E_scratch_t *scratch,
                           mp4_h26x_writer_t *writer, uint8_t *pixels, int64_t positionUs,
                           int64_t bytePosition, unsigned frameDuration90k, int &frames) {
    if(!controls()) { cancelled=true; return false; }
    H264E_io_yuv_t input={};
    input.yuv[0]=pixels;
    input.yuv[1]=pixels+256*144;
    input.yuv[2]=pixels+256*144*5/4;
    input.stride[0]=256; input.stride[1]=128; input.stride[2]=128;
    H264E_run_param_t run={};
    run.encode_speed=H264E_SPEED_FASTEST;
    run.qp_min=run.qp_max=28;
    unsigned char *coded=nullptr;
    int codedBytes=0;
    progress(source,"Encodage",frames,positionUs,bytePosition);
    if(H264E_encode(enc,scratch,&run,&input,&coded,&codedBytes)!=0 || !coded || codedBytes<=0 ||
       mp4_h26x_write_nal(writer,coded,codedBytes,frameDuration90k)<0) {
        lastReason="Encodage H.264 ou ecriture MP4 echoue";
        return false;
    }
    frames++;
    return true;
}
static bool convertOne(const std::string &source, const std::string &output) {
    bool success=false;
    lastReason="MP4 incomplet ou lecture SD impossible";
    AVFormatContext *fmt=nullptr;
    AVIOContext *customIO=nullptr;
    FILE *customFile=nullptr;
    AVCodecContext *decoder=nullptr;
    AVPacket *pkt=nullptr;
    AVFrame *frame=nullptr;
    SwsContext *scaler=nullptr;
    MP4E_mux_t *mux=nullptr;
    mp4_h26x_writer_t writer={};
    FILE *file=nullptr;
    H264E_persist_t *enc=nullptr;
    H264E_scratch_t *scratch=nullptr;
    uint8_t *yuv=nullptr, *heldYuv=nullptr;
    int vi=-1, ai=-1, audioTrack=-1, width=0, height=0, destW=0, destH=0;
    int frames=0, decodedFrames=0;
    bool inputDone=false;
    bool haveHeldFrame=false;
    int64_t firstInputUs=0, lastInputUs=0, nextOutputUs=0, sourceFrameStepUs=0, outputDurationUs=0;
    int64_t targetFrames=INT64_MAX;
    int outputFrameRate=conversionFps;
    int64_t frameStepUs=1000000/conversionFps;
    unsigned frameStep90k=90000/conversionFps;
    currentDurationSeconds=0;
    conversionStarted=osGetTime();
    progress(source,"Ouverture",0,-1,-1,true);

    {
        std::string url="file:"+source;
        fmt=avformat_alloc_context();
        if(!fmt) goto done;
        fmt->probesize=512*1024;
        fmt->max_analyze_duration=AV_TIME_BASE;
        if(avformat_open_input(&fmt,url.c_str(),nullptr,nullptr)<0) {
            if(fmt) avformat_close_input(&fmt);
            customFile=fopen(source.c_str(),"rb");
            uint8_t *buffer=customFile?(uint8_t*)av_malloc(32768):nullptr;
            if(buffer) customIO=avio_alloc_context(buffer,32768,0,customFile,readSdFile,nullptr,seekSdFile);
            if(!customIO) { av_free(buffer); goto done; }
            fmt=avformat_alloc_context(); if(!fmt) goto done;
            fmt->pb=customIO; fmt->flags|=AVFMT_FLAG_CUSTOM_IO;
            fmt->probesize=512*1024;
            fmt->max_analyze_duration=AV_TIME_BASE;
            if(avformat_open_input(&fmt,"local.mp4",nullptr,nullptr)<0) goto done;
        }
    }
    if(!fmt || !fmt->nb_streams) goto done;
    progress(source,"Analyse pistes",0,-1,-1,true);
    currentDurationSeconds=fmt->duration>0 ? (int)(fmt->duration/AV_TIME_BASE) : 0;
    for(unsigned i=0;i<fmt->nb_streams;i++) {
        AVCodecParameters *p=fmt->streams[i]->codecpar;
        if(vi<0 && p->codec_type==AVMEDIA_TYPE_VIDEO) vi=(int)i;
        if(ai<0 && p->codec_type==AVMEDIA_TYPE_AUDIO) ai=(int)i;
    }
    if(vi<0) goto done;
    {
        AVCodecParameters *p=fmt->streams[vi]->codecpar;
        if(p->codec_id!=AV_CODEC_ID_H264 || p->width<16 || p->height<16 || p->width>854 || p->height>480) {
            lastReason="Video H.264 de 480p max requise"; goto done;
        }
        const AVCodec *codec=avcodec_find_decoder(p->codec_id);
        if(!codec) goto done;
        decoder=avcodec_alloc_context3(codec);
        if(!decoder || avcodec_parameters_to_context(decoder,p)<0 || avcodec_open2(decoder,codec,nullptr)<0) goto done;
        width=p->width; height=p->height;
        AVRational rate=fmt->streams[vi]->avg_frame_rate;
        if(rate.num>0 && rate.den>0)
            sourceFrameStepUs=(int64_t)rate.den*1000000/rate.num;
        if(sourceFrameStepUs<=0) sourceFrameStepUs=frameStepUs;
        if(fmt->duration>0 && fmt->duration!=AV_NOPTS_VALUE)
            outputDurationUs=av_rescale_q(fmt->duration,AVRational{1,AV_TIME_BASE},AVRational{1,1000000});
        if(outputDurationUs<=0 && fmt->streams[vi]->duration>0 && fmt->streams[vi]->duration!=AV_NOPTS_VALUE)
            outputDurationUs=av_rescale_q(fmt->streams[vi]->duration,fmt->streams[vi]->time_base,AVRational{1,1000000});
        if(outputDurationUs>0) {
            targetFrames=(outputDurationUs+frameStepUs-1)/frameStepUs;
            if(targetFrames<1) targetFrames=1;
            currentDurationSeconds=(int)(outputDurationUs/1000000);
        }
    }
    if(ai>=0) {
        AVCodecParameters *p=fmt->streams[ai]->codecpar;
        if(p->codec_id!=AV_CODEC_ID_AAC || p->sample_rate<=0 || p->sample_rate>48000 ||
           p->channels<1 || p->channels>2 || !p->extradata || p->extradata_size<2) {
            lastReason="Audio AAC mono/stereo de 48 kHz max requise"; goto done;
        }
    }
    lastReason="Ecriture SD impossible ou espace insuffisant";
    file=fopen(output.c_str(),"wb+"); if(!file) goto done;
    mux=MP4E_open(0,0,file,writeMp4); if(!mux) goto done;
    if(mp4_h26x_write_init(&writer,mux,256,144,0)<0) goto done;
    if(ai>=0) {
        AVCodecParameters *p=fmt->streams[ai]->codecpar;
        MP4E_track_t track={};
        track.track_media_kind=e_audio;
        track.object_type_indication=MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
        track.language[0]='u'; track.language[1]='n'; track.language[2]='d';
        track.time_scale=p->sample_rate; track.default_duration=1024;
        track.u.a.channelcount=p->channels;
        audioTrack=MP4E_add_track(mux,&track);
        if(audioTrack<0 || MP4E_set_dsi(mux,audioTrack,p->extradata,p->extradata_size)<0) goto done;
    }
    {
        H264E_create_param_t cfg={};
        cfg.width=256; cfg.height=144; cfg.gop=outputFrameRate*2; cfg.const_input_flag=1;
        cfg.num_layers=1;
        cfg.max_threads=2; cfg.token=nullptr; cfg.run_func_in_thread=runH264Jobs;
        int persistSize=0,scratchSize=0;
        lastReason="Encodeur H.264 indisponible";
        if(H264E_sizeof(&cfg,&persistSize,&scratchSize)!=0 || persistSize<=0 || scratchSize<=0) goto done;
        enc=(H264E_persist_t*)memalign(64,persistSize);
        scratch=(H264E_scratch_t*)memalign(64,scratchSize);
        yuv=(uint8_t*)memalign(64,256*144*3/2);
        heldYuv=(uint8_t*)memalign(64,256*144*3/2);
        lastReason="Memoire insuffisante pour encoder";
        if(!enc || !scratch || !yuv || !heldYuv || H264E_init(enc,&cfg)!=0) goto done;
    }
    // Keep aspect ratio, using even YUV420 dimensions and black borders.
    destW=width*144/height; destH=144;
    if(destW>256) { destW=256; destH=height*256/width; }
    destW=std::max(2,destW&~1); destH=std::max(2,destH&~1);
    pkt=av_packet_alloc(); frame=av_frame_alloc(); if(!pkt || !frame) goto done;
    progress(source,"Decodage",0,0,0,true);
    while(controls()) {
        int readResult=av_read_frame(fmt,pkt);
        bool drainVideo=false;
        if(readResult==AVERROR_EOF) {
            if(avcodec_send_packet(decoder,nullptr)<0) goto done;
            inputDone=true;
            drainVideo=true;
        } else if(readResult<0) goto done;
        // Successful reads reach the packet handlers below. Keep these
        // handlers out of the EOF/error branches or no input packets are used.
        else {
            int64_t positionUs=-1;
            if(pkt->pts!=AV_NOPTS_VALUE && pkt->stream_index==vi)
                positionUs=av_rescale_q(pkt->pts,fmt->streams[pkt->stream_index]->time_base,AVRational{1,1000000});
            int64_t bytePosition=fmt->pb ? avio_tell(fmt->pb) : -1;
            progress(source,"Lecture MP4",frames,positionUs,bytePosition);
            if(pkt->stream_index==ai && audioTrack>=0) {
                int duration=(int)av_rescale_q(pkt->duration,fmt->streams[ai]->time_base,AVRational{1,fmt->streams[ai]->codecpar->sample_rate});
                if(duration<=0) duration=1024;
                if(MP4E_put_sample(mux,audioTrack,pkt->data,pkt->size,duration,MP4E_SAMPLE_DEFAULT)<0) { av_packet_unref(pkt); goto done; }
            } else if(pkt->stream_index==vi) {
                if(avcodec_send_packet(decoder,pkt)<0) { av_packet_unref(pkt); goto done; }
                drainVideo=true;
            }
        }
        if(drainVideo) {
            while(avcodec_receive_frame(decoder,frame)>=0) {
                if(!scaler) {
                    scaler=sws_getContext(frame->width,frame->height,(AVPixelFormat)frame->format,
                                          destW,destH,AV_PIX_FMT_YUV420P,SWS_BILINEAR,nullptr,nullptr,nullptr);
                    if(!scaler) { lastReason="Image source non convertible"; av_frame_unref(frame); av_packet_unref(pkt); goto done; }
                }
                int64_t ts=frame->best_effort_timestamp;
                int64_t absoluteUs=ts==AV_NOPTS_VALUE
                    ? (haveHeldFrame ? firstInputUs+(int64_t)decodedFrames*sourceFrameStepUs : 0)
                    : av_rescale_q(ts,fmt->streams[vi]->time_base,AVRational{1,1000000});
                if(!haveHeldFrame) firstInputUs=absoluteUs;
                int64_t us=absoluteUs-firstInputUs;
                if(us<0) us=0;
                if(haveHeldFrame && us<=lastInputUs) {
                    decodedFrames++;
                    av_frame_unref(frame);
                    continue;
                }
                memset(yuv,16,256*144); memset(yuv+256*144,128,256*144/2);
                int x=(256-destW)/2, y=(144-destH)/2;
                uint8_t *dst[4]={yuv+y*256+x,yuv+256*144+(y/2)*128+x/2,yuv+256*144*5/4+(y/2)*128+x/2,nullptr};
                int strides[4]={256,128,128,0};
                sws_scale(scaler,frame->data,frame->linesize,0,frame->height,dst,strides);
                int64_t bytePosition=fmt->pb?avio_tell(fmt->pb):-1;
                if(!haveHeldFrame) {
                    std::swap(yuv,heldYuv);
                    haveHeldFrame=true;
                    lastInputUs=us;
                    if(frames<targetFrames && !encodeYuvFrame(source,enc,scratch,&writer,heldYuv,0,bytePosition,frameStep90k,frames)) {
                        av_frame_unref(frame); av_packet_unref(pkt); goto done;
                    }
                    nextOutputUs=frameStepUs;
                } else {
                    int64_t midpoint=lastInputUs+(us-lastInputUs)/2;
                    while(nextOutputUs<=midpoint && frames<targetFrames) {
                        int64_t outputUs=(int64_t)frames*frameStepUs;
                        if(!encodeYuvFrame(source,enc,scratch,&writer,heldYuv,outputUs,bytePosition,frameStep90k,frames)) {
                            av_frame_unref(frame); av_packet_unref(pkt); goto done;
                        }
                        nextOutputUs+=frameStepUs;
                    }
                    std::swap(yuv,heldYuv);
                    lastInputUs=us;
                }
                decodedFrames++;
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
        if(inputDone) break;
    }
    if(!cancelled && haveHeldFrame) {
        if(outputDurationUs<=0) outputDurationUs=lastInputUs+sourceFrameStepUs;
        targetFrames=(outputDurationUs+frameStepUs-1)/frameStepUs;
        if(targetFrames<1) targetFrames=1;
        while(frames<targetFrames) {
            int64_t outputUs=(int64_t)frames*frameStepUs;
            if(!encodeYuvFrame(source,enc,scratch,&writer,heldYuv,outputUs,fmt&&fmt->pb?avio_tell(fmt->pb):-1,frameStep90k,frames))
                goto done;
        }
    }
    if(!cancelled && frames>0 && frames>=targetFrames) success=true;
done:
    if(success) progress(source,"Finalisation",frames,-1,-1,true);
    if(pkt) av_packet_free(&pkt);
    if(frame) av_frame_free(&frame);
    if(scaler) sws_freeContext(scaler);
    if(decoder) avcodec_free_context(&decoder);
    if(fmt) avformat_close_input(&fmt);
    if(customIO) { av_freep(&customIO->buffer); avio_context_free(&customIO); }
    if(customFile) fclose(customFile);
    if(enc) free(enc);
    if(scratch) free(scratch);
    if(yuv) free(yuv);
    if(heldYuv) free(heldYuv);
    if(mux) {
        mp4_h26x_write_close(&writer);
        if(MP4E_close(mux)<0) { success=false; lastReason="Finalisation MP4 impossible"; }
    }
    if(file) {
        bool writeError=fflush(file)!=0 || ferror(file);
        if(fclose(file)!=0) writeError=true;
        if(writeError) { success=false; lastReason="Ecriture SD incomplete"; }
    }
    return success;
}
static bool confirmHeavy(const std::string &path,const MediaInfo &info) {
    bool heavy=info.bytes>=128ULL*1024*1024 || info.seconds>=15*60 ||
               (info.height>=360 && info.seconds>=5*60);
    if(!heavy) return true;
    progressPercent=0;
    char detail[100];
    snprintf(detail,sizeof(detail),"%u Mo | %d x %d | %lld min | %d fps",(unsigned)(info.bytes>>20),info.width,info.height,(long long)(info.seconds/60),conversionFps);
    top("Conversion longue",path.substr(0,38).c_str(),"A : confirmer   B : ignorer");
    consoleSelect(&lower); consoleClear();
    printf("\x1b[33mEtes-vous vraiment sur ?\x1b[0m\n\nLa conversion risque de prendre beaucoup de temps en raison de sa taille.\n\n%s\n\nA : convertir\nB : ignorer ce fichier\nSTART : annuler\n",detail);
    while(aptMainLoop()) {
        hidScanInput();
        u32 keys=hidKeysDown();
        if(keys&KEY_START) { cancelled=true; return false; }
        if(keys&KEY_B) return false;
        if(keys&KEY_A) return true;
        gspWaitForVBlank();
    }
    cancelled=true;
    return false;
}
static bool chooseOutputFps(int &fps) {
    top("Cadence de sortie","A : 15 fps (charge reduite)","X : 24 fps (plus fluide) | B : retour");
    consoleSelect(&lower); consoleClear();
    printf("\x1b[36mCADENCE DE CONVERSION\x1b[0m\n\nA : 15 fps\nCharge reduite, mouvement moins fluide.\n\nX : 24 fps\nMouvement plus fluide, charge plus elevee.\n\nB : retour a l'explorateur\nSTART : quitter\n");
    while(aptMainLoop()) {
        hidScanInput();
        u32 keys=hidKeysDown();
        if(keys&KEY_START) { cancelled=true; return false; }
        if(keys&KEY_B) return false;
        if(keys&KEY_A) { fps=15; return true; }
        if(keys&KEY_X) { fps=24; return true; }
        gspWaitForVBlank();
    }
    cancelled=true;
    return false;
}
static void runConversions(const std::vector<std::string> &files) {
    cancelled=false; converted=skipped=declined=failed=0; progressPercent=0;
    if(files.empty()) {
        top("Aucun MP4 compatible","H.264/AAC de plus de 144p","480p maximum");
        return;
    }
    int chosenFps=0;
    if(!chooseOutputFps(chosenFps)) return;
    HomeActionGuard homeGuard;
    conversionFps=chosenFps;
    if(mkdir(OUT_DIR,0777)!=0) {
        struct stat st;
        if(stat(OUT_DIR,&st)!=0 || !S_ISDIR(st.st_mode)) {
            top("Carte SD inaccessible","Dossier SHIELD_MP4 impossible","Verifier l'espace libre et la SD");
            return;
        }
    }
    beginConversionResources();
    for(size_t i=0;i<files.size() && !cancelled;i++) {
        MediaInfo info=probeMp4(files[i]);
        if(!info.eligible) { failed++; lastReason="MP4 incompatible ou de 144p maximum"; continue; }
        progressPercent=(unsigned)(i*100/std::max((size_t)1,files.size()));
        char position[80];
        snprintf(position,sizeof(position),"Fichier %u / %u",(unsigned)(i+1),(unsigned)files.size());
        top("Verification des doublons",files[i].substr(0,38).c_str(),position);
        uint64_t bytes=0,hash=fingerprint(files[i],bytes);
        if(!hash || !bytes) { lastReason="Source SD inaccessible"; failed++; continue; }
        if(outputContainsFingerprint(hash,bytes)) { skipped++; continue; }
        char oldStem[80]; snprintf(oldStem,sizeof(oldStem),"%s/%016llX-%llX.mp4",OUT_DIR,(unsigned long long)hash,(unsigned long long)bytes);
        struct stat st;
        if(stat(oldStem,&st)==0) { skipped++; continue; }
        std::string final,temp,fingerprintPath;
        if(!chooseOutputPaths(files[i],final,temp,fingerprintPath)) { lastReason="Trop de fichiers du meme nom"; failed++; continue; }
        if(!confirmHeavy(files[i],info)) { if(!cancelled) declined++; continue; }
        remove(temp.c_str()); lastDraw=0; progressPercent=0; sourceBytes=bytes;
        bool ok=convertOne(files[i],temp);
        if(ok && rename(temp.c_str(),final.c_str())==0) {
            converted++;
            if(!writeFingerprint(fingerprintPath,hash,bytes)) lastReason="Index des doublons non enregistre";
        }
        else { if(ok) lastReason="Renommage SD impossible"; remove(temp.c_str()); failed++; }
    }
    endConversionResources();
    char summary[100]; snprintf(summary,sizeof(summary),"%u OK | %u doublons | %u ignores | %u erreurs",converted,skipped,declined,failed);
    progressPercent=cancelled?progressPercent:100;
    top(cancelled?"Conversion annulee":"Conversion terminee",summary,
        failed?helpFor(lastReason):"Sortie : /SHIELD_MP4");
}
int main() {
    gfxInitDefault(); consoleInit(GFX_BOTTOM,&lower);
    aptSetHomeAllowed(true);
    // Keep the console awake through browsing, SD scans, and conversion.
    // Restore normal sleep handling only when the application exits.
    aptSetSleepAllowed(false);
    disableWifiForApp();
    gfxSet3D(false);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    topTarget=C2D_CreateScreenTarget(GFX_TOP,GFX_LEFT);
    uiText=C2D_TextBufNew(1024);
    hidScanInput();
    const bool forceLanguage=(hidKeysHeld()&KEY_X)!=0;
    const bool reviewGuide=(hidKeysHeld()&KEY_SELECT)!=0;
    if(!chooseLanguage(forceLanguage) || !showFirstRunGuide(reviewGuide || forceLanguage)) cancelled=true;
    if(cancelled) goto app_cleanup;
    top("Explorateur SD","Ouverture des dossiers...","A : fichier   Y : toute la SD");
    loadCurrentDirectory();
    showBrowser();
    top("Explorateur SD","Choisir un fichier MP4","A : fichier  |  Y : toute la SD");
    while(aptMainLoop() && !cancelled) {
        hidScanInput(); u32 down=hidKeysDown(), repeat=hidKeysDownRepeat();
        if(down&KEY_START) break;
        bool changed=false;
        if(!entries.empty()) {
            if(repeat&KEY_UP) { selected=(selected+entries.size()-1)%entries.size(); changed=true; }
            else if(repeat&KEY_DOWN) { selected=(selected+1)%entries.size(); changed=true; }
            else if(repeat&KEY_LEFT) { selected=selected>=16?selected-16:0; changed=true; }
            else if(repeat&KEY_RIGHT) { selected=std::min(selected+16,entries.size()-1); changed=true; }
        }
        if(down&KEY_B) {
            if(!browserHistory.empty()) {
                BrowserSnapshot &parent=browserHistory.back();
                currentDir=parent.path; entries.swap(parent.entries); selected=parent.selected;
                browserReadError=false; browserHistory.pop_back(); changed=true;
            }
        }
        if((down&KEY_A) && !entries.empty()) {
            BrowserEntry entry=entries[selected];
            if(entry.directory) {
                browserHistory.emplace_back();
                browserHistory.back().path=currentDir;
                browserHistory.back().selected=selected;
                browserHistory.back().entries.swap(entries);
                currentDir=entry.path;
                top("Explorateur SD","Recherche des MP4...","Patienter");
                loadCurrentDirectory(); changed=true;
            } else {
                runConversions(std::vector<std::string>{entry.path}); changed=true;
            }
        }
        if(down&KEY_Y) {
            std::vector<std::string> files;
            scanVisited=0;
            top("Recherche SD","Liste des MP4...","START : annuler");
            {
                HomeActionGuard homeGuard;
                collect("sdmc:/",files,0);
            }
            if(!cancelled) runConversions(files);
            changed=true;
        }
        if(changed && !cancelled) showBrowser();
        gspWaitForVBlank();
    }
app_cleanup:
    endConversionResources();
    restoreWifiAfterApp();
    if(uiText) C2D_TextBufDelete(uiText);
    C2D_Fini(); C3D_Fini(); gfxExit();
    aptSetSleepAllowed(true);
    aptSetHomeAllowed(true);
    return 0;
}
