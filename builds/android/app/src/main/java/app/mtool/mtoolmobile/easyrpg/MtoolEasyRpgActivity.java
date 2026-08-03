package app.mtool.mtoolmobile.easyrpg;

import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.ActionMode;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SearchEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;

import org.json.JSONArray;
import org.json.JSONObject;
import org.easyrpg.player.player.EasyRpgPlayerActivity;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

import java.util.Arrays;

import app.mtool.mtoolmobile.easyrpg.services.GeckoCrashHandlerService;
import app.mtool.mtoolmobile.mtool.managers.MToolViewManager;
import app.mtool.mtoolmobile.mtool.runtime.MToolRuntimeLoader;

/**
 * MTool 集成入口：由 MToolMobile 拉起（nextStartArg 指定包名 + 本类名）。
 * 启动参数（游戏路径、MTool 端口）从主包私有目录读取；也支持用 intent extra
 * 直接传入（mtool_port / command_line），方便 adb 调试。
 */
public class MtoolEasyRpgActivity extends EasyRpgPlayerActivity {
    private static final String TAG = "MToolEasyRPG";
    private MToolViewManager mtoolViewManager;
    private static final String EASYRPG_LOCAL_VFS_SNAPSHOT_FILE = "node/mtoolWebRoot/easyrpg_vfs_local_snapshot.json";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        mtoolViewManager = new MToolViewManager(this, GeckoCrashHandlerService.class);
        boolean paramsOk = prepareLaunchParams();
        Exception vfsRestoreError = null;
        try {
            restorePluginVfs();
        } catch (Exception e) {
            vfsRestoreError = e;
            Log.e(TAG, "恢复插件 VFS 失败", e);
        }
        // super.onCreate 必须无条件调用，否则框架抛 SuperNotCalledException
        super.onCreate(savedInstanceState);
        if (vfsRestoreError != null) {
            new AlertDialog.Builder(this)
                    .setTitle("MTool")
                    .setMessage("恢复游戏文件系统失败")
                    .setCancelable(false)
                    .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                        mtoolViewManager.justKillMeAndBringUpMTOOL();
                        finish();
                    })
                    .show();
            return;
        }
        if (!paramsOk) {
            Log.e(TAG, "读取游戏路径或端口失败，可能是 MTool 服务未在运行");
            new AlertDialog.Builder(this)
                    .setTitle("MTool")
                    .setMessage("读取游戏路径或端口失败，可能是 MTool 服务未在运行")
                    .setCancelable(false)
                    .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                        mtoolViewManager.justKillMeAndBringUpMTOOL();
                        finish();
                    })
                    .show();
            return;
        }

        if (mBrokenLibraries) {
            return;
        }
        embedMtoolUi();
    }

    /**
     * 在 EasyRpgPlayerActivity.onCreate 读取 intent extra 之前把参数补齐。
     * extra 齐全（adb 调试拉起）时直接用，否则从 MToolMobile 私有目录读。
     */
    private boolean prepareLaunchParams() {
        Intent intent = getIntent();
        String port = intent.getStringExtra(TAG_MTOOL_PORT);
        String[] cmdline = intent.getStringArrayExtra(TAG_COMMAND_LINE);
        if (port != null && !port.isEmpty() && cmdline != null) {
            return true;
        }

        String serverUri = PackageContextHelper.readFileFromOtherApp(this, "node/serverURI");
        String gamePath = PackageContextHelper.readFileFromOtherApp(this, "node/mtoolWebRoot/easyrpg_game_path");
        if (serverUri == null || gamePath == null) {
            Log.e(TAG, "serverUri or gamePath is null");
            return false;
        }
        Log.d(TAG, "serverUri: " + serverUri);
        Log.d(TAG, "gamePath: " + gamePath);
        try {
            port = serverUri.trim().split(":")[1].trim();
        } catch (Exception e) {
            return false;
        }
        Log.d(TAG, "port: " + port);
        intent.putExtra(TAG_MTOOL_PORT, port);
        if (cmdline == null) {
            cmdline = new String[]{"--project-path", gamePath.trim()};
        }
        Log.d(TAG, "cmdline: " + Arrays.toString(cmdline));
        intent.putExtra(TAG_COMMAND_LINE, cmdline);
        return true;
    }

    private void restorePluginVfs() throws Exception {
        JSONArray callerLibs = new JSONArray();
        callerLibs.put("libeasyrpg_android.so");
        callerLibs.put("libgamebrowser.so");
        callerLibs.put("libSDL3.so");
        JSONObject params = new JSONObject();
        params.put("snapshotRelPath", EASYRPG_LOCAL_VFS_SNAPSHOT_FILE);
        params.put("serverUriRelPath", "node/serverURI");
        params.put("callerLibs", callerLibs);
        MToolRuntimeLoader.restorePluginVfs(this, params.toString());
    }

    /**
     * native 侧 FileFinder_RTP 通过 JNI 调这个方法取 RTP 根目录（内部会自己拼
     * /2000 或 /2003）。返回 VFS 里 assets 挂载点的路径：RTP 打在插件 APK 的
     * assets/RTP_EasyRPG 下，由 VFS 挂载提供，不落盘。路径由主包侧
     * gameLibEasyRPG.js 在启动时写入。
     */
    @Override
    public String getRtpPath() {
        try {
            String rtpPath = PackageContextHelper.readFileFromOtherApp(this, "node/mtoolWebRoot/easyrpg_rtp_path");
            if (rtpPath != null && !rtpPath.trim().isEmpty()) {
                Log.d(TAG, "rtpPath: " + rtpPath.trim());
                return rtpPath.trim();
            }
        } catch (Exception e) {
            Log.e(TAG, "读取 RTP 路径失败", e);
        }
        return "";
    }

    /**
     * 把播放器内容整体挪进 MToolViewManager 的根布局（游戏在下，工具 UI 浮在上）。
     * 注意 init() 内部会 activity.setContentView(root)，所以先把播放器视图摘出来，
     * init 完再插回根布局底部——顺序反了会把 root 添加进它自己，无限递归。
     */
    private void embedMtoolUi() {
        try {
            ViewGroup content = findViewById(android.R.id.content);
            View playerView = (content != null && content.getChildCount() > 0) ? content.getChildAt(0) : null;
            if (playerView == null) {
                Log.e(TAG, "找不到播放器内容视图");
                return;
            }
            content.removeView(playerView);

            mtoolViewManager.defaultToGameView = true;
            ViewGroup root = mtoolViewManager.init();
            if (root == null) {
                Log.e(TAG, "MToolViewManager init 失败");
                setContentView(playerView);
                return;
            }
            root.addView(playerView, 0);
            mtoolViewManager.setGameInputTarget(new vInputRecv());
        } catch (Exception e) {
            Log.e(TAG, "嵌入 MTool UI 失败", e);
        }
    }

    // MTool runtime 反射调用
    public void releaseGameView() {
        if (mtoolViewManager != null) {
            mtoolViewManager.justKillMeAndBringUpMTOOL();
        }
    }

    // MTool runtime 反射调用
    public boolean handleGameInit(String startArgsString) {
        return false;
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        try {
            if (mtoolViewManager != null
                    && mtoolViewManager.getUiManager() != null
                    && mtoolViewManager.getUiManager().dispatchTouchEvent(ev)) {
                return true;
            }
        } catch (Exception e) {
            Log.e(TAG, "分发触摸事件失败", e);
        }
        return super.dispatchTouchEvent(ev);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mtoolViewManager != null) {
            try {
                if (mtoolViewManager.onHardwareKeyEvent(event)) {
                    return true;
                }
            } catch (Exception e) {
                Log.e(TAG, "分发硬件按键事件失败", e);
            }
        }
        try {
            MToolViewManager.sendKeyEventToSender(event);
        } catch (Exception e) {
            Log.e(TAG, "发送按键事件失败", e);
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (mtoolViewManager != null) {
            try {
                if (mtoolViewManager.onHardwareMotionEvent(event)) {
                    return true;
                }
            } catch (Exception e) {
                Log.e(TAG, "分发硬件 MotionEvent 失败", e);
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mtoolViewManager != null) {
            mtoolViewManager.onResume();
        }
    }

    @Override
    protected void onPause() {
        if (mtoolViewManager != null) {
            mtoolViewManager.onPause();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (mtoolViewManager != null) {
            mtoolViewManager.onDestroy();
        }
        super.onDestroy();
    }

    @Override
    protected void onNewIntent(final Intent intent) {
        super.onNewIntent(intent);
        if (mtoolViewManager != null && intent != null) {
            mtoolViewManager.onNewIntent(intent);
        }
    }
    @Override
    public void showInputLayout() {
    }

    class vInputRecv implements Window.Callback {
        @Override
        public boolean dispatchTouchEvent(MotionEvent event) {
            return false;
        }

        @Override
        public boolean dispatchTrackballEvent(MotionEvent motionEvent) {
            return false;
        }

        @Override
        public boolean dispatchKeyEvent(KeyEvent event) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                SDLActivity.onNativeKeyDown(event.getKeyCode());
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                SDLActivity.onNativeKeyUp(event.getKeyCode());
            }
            return false;
        }

        @Override
        public boolean dispatchKeyShortcutEvent(KeyEvent keyEvent) {
            return false;
        }

        @Override
        public boolean dispatchGenericMotionEvent(MotionEvent event) {
            float x, y;
            int action;

            switch (event.getSource()) {
                case InputDevice.SOURCE_JOYSTICK:
                    return SDLControllerManager.handleJoystickMotionEvent(event);

                case InputDevice.SOURCE_MOUSE:
                    action = event.getActionMasked();
                    switch (action) {
                        case MotionEvent.ACTION_SCROLL:
                            x = event.getAxisValue(MotionEvent.AXIS_HSCROLL, 0);
                            y = event.getAxisValue(MotionEvent.AXIS_VSCROLL, 0);
                            SDLActivity.onNativeMouse(0, action, x, y, false);
                            return true;

                        case MotionEvent.ACTION_HOVER_MOVE:
                            x = event.getX(0);
                            y = event.getY(0);
                            SDLActivity.onNativeMouse(0, action, x, y, false);
                            return true;
                        case MotionEvent.ACTION_BUTTON_PRESS:
                            x = event.getX(0);
                            y = event.getY(0);
                            SDLActivity.onNativeMouse(event.getActionButton(), MotionEvent.ACTION_DOWN, x, y, false);
                            return true;
                        case MotionEvent.ACTION_BUTTON_RELEASE:
                            x = event.getX(0);
                            y = event.getY(0);
                            SDLActivity.onNativeMouse(0, MotionEvent.ACTION_UP, x, y, false);
                            return true;

                        default:
                            break;
                    }
                    break;

                default:
                    break;
            }
            return false;
        }

        @Override
        public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent accessibilityEvent) {
            return false;
        }

        @Override
        public View onCreatePanelView(int i) {
            return null;
        }

        @Override
        public boolean onCreatePanelMenu(int i, Menu menu) {
            return false;
        }

        @Override
        public boolean onPreparePanel(int i, View view, Menu menu) {
            return false;
        }

        @Override
        public boolean onMenuOpened(int i, Menu menu) {
            return false;
        }

        @Override
        public boolean onMenuItemSelected(int i, MenuItem menuItem) {
            return false;
        }

        @Override
        public void onWindowAttributesChanged(WindowManager.LayoutParams layoutParams) {
        }

        @Override
        public void onContentChanged() {
        }

        @Override
        public void onWindowFocusChanged(boolean b) {
        }

        @Override
        public void onAttachedToWindow() {
        }

        @Override
        public void onDetachedFromWindow() {
        }

        @Override
        public void onPanelClosed(int i, Menu menu) {
        }

        @Override
        public boolean onSearchRequested() {
            return false;
        }

        @Override
        public boolean onSearchRequested(SearchEvent searchEvent) {
            return false;
        }

        @Override
        public ActionMode onWindowStartingActionMode(ActionMode.Callback callback) {
            return null;
        }

        @Override
        public ActionMode onWindowStartingActionMode(ActionMode.Callback callback, int i) {
            return null;
        }

        @Override
        public void onActionModeStarted(ActionMode actionMode) {
        }

        @Override
        public void onActionModeFinished(ActionMode actionMode) {
        }
    }
}
