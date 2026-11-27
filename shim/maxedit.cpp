// "Edit" on a Max for Live device (Live 12.4.7, Max 9.1.5). LIVE_MAX_EDIT=0 leaves the note
// instead; otherwise the device opens in the bundled Linux Max editor (Resources/Max/Max)
// through shim/max-editor. On by default: verified by editing and saving a device from Live.
// The editor asks for an Ableton account the first time, and Max's own
// start-up aborts now and then, so the first attempt can fail and a second one works.
//
// Live calls Wires::EditWithPreview in the Max runtime's libpatcher.so. With no Max editor
// process running it calls WiresPrivate::mfl_device_edit, which launches
// WiresUtil::get_editor_path() with --mfl. The Linux runtime's path is a folder, so the
// launch fails, the call returns -1 and Live turns that into a fatal
// "serious program error" (Log.txt: The Max function "EditWithPreview" returned with
// error -1). The function is wrapped once libpatcher.so is loaded (with the first Max
// device; its prologue is checked, so another Max build is left alone): an error becomes
// a note and "no error", and the editor is only launched when its path is a program (see
// editorLaunchable). With LIVE_MAX_EDIT=1 the editor is Live's own Linux Max (Resources/Max/Max),
// started through shim/max-editor, which preloads libmaxstatfix.so (see maxstatfix.c). Both
// sides get working shared memory for the device's audio (maxshmem.c).
#include "livebuild.h"
#include <QMessageBox>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len,
                   void *replacement);                             // hook.cpp
extern "C" int maxShmemInstall(void *(*lookup)(const char *name)); // maxshmem.c

namespace {

// push r14; push rbx; sub rsp, 0x828; mov ebx, 1
const unsigned char kPrologue[] = {0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x28, 0x08,
                                   0x00, 0x00, 0xbb, 0x01, 0x00, 0x00, 0x00};
constexpr int kMaxAttempts = 100; // view passes after the Max runtime is seen

using EditFn = int (*)(void *device);
EditFn originalEdit;

void showNote() {
  static QMessageBox *box;
  if (!box) {
    box = new QMessageBox(
        QMessageBox::Information, QStringLiteral("Edit in Max"),
        QStringLiteral("This Linux build of Live has no Max editor, so Max for Live devices "
                       "cannot be edited here. The device keeps working."),
        QMessageBox::Ok);
    box->setModal(false);
  }
  box->show();
  box->raise();
}

using PathFn = char *(*)();
PathFn editorPath;       // WiresUtil::get_editor_path(): cached in s_editor_path, 2048-byte buffer
char **cachedEditorPath; // s_editor_path
std::string launcher;    // shim/max-editor, with LIVE_MAX_EDIT=1

bool isExecutableFile(const char *path) {
  struct stat st;
  return path && !stat(path, &st) && S_ISREG(st.st_mode) && !access(path, X_OK);
}

// The runtime's editor path is Live's Resources/Max folder (a Mac-style app path) or, on
// some starts, none: launching the folder forks Live and the execv fails, leaving a copy
// of Live running. So the path is checked first; with LIVE_MAX_EDIT=1 it is shim/max-editor.
bool editorLaunchable() {
  char *path = editorPath();
  if (!launcher.empty() && cachedEditorPath && (!path || launcher != path)) {
    static auto newPtr = reinterpret_cast<char *(*)(long)>(dlsym(RTLD_DEFAULT, "sysmem_newptr"));
    if (!path && newPtr)
      path = *cachedEditorPath = newPtr(2048);
    if (path) {
      strcpy(path, launcher.c_str()); // shorter than 2048 (checked in maxEditInstall)
      menufixTrace("maxedit: editor path set to shim/max-editor\n");
    }
  }
  return isExecutableFile(path);
}

int editWithPreview(void *device) {
  if (!editorLaunchable()) {
    menufixTrace("maxedit: no Max editor to launch, edit ignored\n");
    showNote();
    return 0;
  }
  const int error = originalEdit(device);
  if (error) {
    menufixTrace("maxedit: EditWithPreview returned %d, ignored\n", error);
    showNote();
  }
  return 0; // Max's "no error": Live carries on
}

} // namespace

// Called from maxviews.cpp's view pass while a Max device is shown; cheap once done.
void maxEditInstall() {
  static int attempts;
  if (attempts >= kMaxAttempts)
    return;
  if (attempts == 0) {
    const char *off = getenv("LIVE_MAX_EDIT");
    if ((off && *off == '0') || liveBuild() != LiveBuild::Live12_4_7) {
      attempts = kMaxAttempts;
      return;
    }
  }
  // libpatcher.so has no soname: find it next to libMaxPlugLib.so, which exports zgetfn.
  Dl_info info;
  void *zgetfn = dlsym(RTLD_DEFAULT, "zgetfn");
  if (!zgetfn || !dladdr(zgetfn, &info) || !info.dli_fname)
    return; // the Max runtime is not loaded yet
  ++attempts;
  std::string path(info.dli_fname);
  path.replace(path.rfind('/') + 1, std::string::npos, "libpatcher.so");
  void *lib = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (!lib)
    return;
  attempts = kMaxAttempts;
  void *fn = dlsym(lib, "_ZN5Wires15EditWithPreviewEP14plug_DeviceRef");
  editorPath = reinterpret_cast<PathFn>(dlsym(lib, "_ZN9WiresUtil15get_editor_pathEv"));
  originalEdit = fn && editorPath
                     ? reinterpret_cast<EditFn>(hookFunction(
                           reinterpret_cast<uintptr_t>(fn), kPrologue, sizeof kPrologue,
                           reinterpret_cast<void *>(&editWithPreview)))
                     : nullptr;
  if (!originalEdit) {
    menufixTrace("maxedit: unknown libpatcher.so, left alone\n");
  } else {
    cachedEditorPath = static_cast<char **>(dlsym(lib, "s_editor_path"));
    const char *on = getenv("LIVE_MAX_EDIT");
    Dl_info self;
    if ((!on || *on != '0') && dladdr(reinterpret_cast<void *>(&maxEditInstall), &self) &&
        self.dli_fname) {
      const std::string shim(self.dli_fname);
      const std::string script = shim.substr(0, shim.rfind('/') + 1) + "max-editor";
      const std::string editor = path.substr(0, path.rfind("/resources/support/")) + "/Max";
      if (script.size() < 2048 && isExecutableFile(script.c_str()) &&
          isExecutableFile(editor.c_str())) {
        setenv("LIVE_MAX_EDITOR", editor.c_str(), 1); // read by shim/max-editor
        launcher = script;
        // The editor runs the device's audio through shared memory, a stub in this runtime.
        const int n = maxShmemInstall([](const char *name) { return dlsym(RTLD_DEFAULT, name); });
        menufixTrace("maxedit: shared memory for the editor: %d of 3 functions\n", n);
      }
    }
    menufixTrace("maxedit: Edit in Max guarded\n");
  }
  dlclose(lib);
}
