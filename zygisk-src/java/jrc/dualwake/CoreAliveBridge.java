package jrc.dualwake;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * Registers VoiceAssist's official CoreAlive lifecycle on myron while another
 * package remains the default voice-interaction service.
 *
 * VoiceAssist 7.12.2.0318 normally calls CoreAliveManager.registerAlive() from
 * AssistInteractionService.onReady(). Android never calls onReady() while
 * Google is the default assistant, so the independent VoiceTrigger service is
 * not bound after a cold boot. This bridge reuses the app process started by
 * VoiceAssist's own exported boot receiver as the trigger and registers its
 * own CoreAlive manager. No role, voiceprint, package data, or SoundTrigger
 * API is changed here.
 *
 * 由 Zygisk 在 com.miui.voiceassist:voice_trigger 進程注入執行；
 * 全程 try/catch，任何一步失敗只記 log、安全 no-op。
 */
public final class CoreAliveBridge {
    private static final String TAG = "DualWake-CoreAlive";
    private static final String TARGET_DEVICE = "myron";
    private static final String CORE_ALIVE_MANAGER = "com.xiaomi.assist.biz.CoreAliveManager";
    private static final String CORE_ALIVE_CALLBACK = "com.xiaomi.assist.biz.CoreAliveManager$a";
    private static final String VOICE_TRIGGER_ENABLED = "voice_trigger_enabled";

    private static volatile boolean registered = false;

    private CoreAliveBridge() {}

    /** Called from the Zygisk native worker once the Application exists. */
    public static void registerOnMain(final Context app) {
        try {
            if (!TARGET_DEVICE.equals(Build.DEVICE)) {
                Log.i(TAG, "skip: device is " + Build.DEVICE);
                return;
            }
            if (Settings.Global.getInt(
                    app.getContentResolver(), VOICE_TRIGGER_ENABLED, 0) != 1) {
                Log.i(TAG, "skip: voice_trigger_enabled is off");
                return;
            }
            String interactionService = Settings.Secure.getString(
                    app.getContentResolver(), "voice_interaction_service");
            if (interactionService != null
                    && interactionService.startsWith("com.miui.voiceassist/")) {
                Log.i(TAG, "skip: XiaoAI is already the default assistant");
                return;
            }
            // 與原本 BootupReceiver.onReceive 的執行緒語意一致：在主執行緒呼叫。
            // 主 Looper 常駐，CoreAlive 內部延遲約 2 秒的 bind 不會因執行緒
            // 結束而中斷。
            new Handler(Looper.getMainLooper()).post(new Runnable() {
                @Override
                public void run() {
                    doRegister(app);
                }
            });
        } catch (Throwable t) {
            Log.e(TAG, "registerOnMain failed", t);
        }
    }

    private static void doRegister(Context app) {
        if (registered) {
            Log.i(TAG, "skip: CoreAlive already registered in this process");
            return;
        }
        // 這個进程是 cached empty process，MIUI 可能在 onReceive 結束後數十
        // 毫秒內回收；先發出最關鍵的 bind IPC。這正是 CoreAlive 2 秒後會做的
        // 同一個官方綁定（同 action、同 component、caller 同為小愛），且
        // VoiceTriggerService 綁定後會自己 startForeground 常駐。
        try {
            Intent intent = new Intent("com.miui.voiceassist.action.CoreAliveService")
                    .setComponent(new ComponentName(
                            "com.miui.voicetrigger",
                            "com.miui.voicetrigger.VoiceTriggerService"));
            boolean bound = app.bindService(intent, new BootBindConnection(),
                    Context.BIND_AUTO_CREATE);
            Log.i(TAG, "direct VoiceTriggerService bind -> " + bound);
        } catch (Throwable t) {
            Log.e(TAG, "direct bind failed", t);
        }
        try {
            ClassLoader classLoader = app.getClassLoader();
            Class<?> managerClass = classLoader.loadClass(CORE_ALIVE_MANAGER);
            Class<?> callbackClass = classLoader.loadClass(CORE_ALIVE_CALLBACK);
            Field singletonField = managerClass.getDeclaredField("a");
            Method registerAlive = managerClass.getDeclaredMethod(
                    "registerAlive", Context.class, callbackClass);
            Object manager = singletonField.get(null);
            if (manager == null) {
                Log.e(TAG, "CoreAliveManager singleton is null");
                return;
            }
            registerAlive.invoke(manager, new Object[]{app.getApplicationContext(), null});
            registered = true;
            Log.i(TAG, "CoreAlive registration invoked for myron VoiceTrigger");

            // registerAlive 內部把真正的 bind 排在主 Looper 2 秒後
            // （sendEmptyMessageDelayed(1000, 2000)）。這個进程是 cached
            // empty process，MIUI 常在 1 秒內回收，導致 bind 來不及執行。
            // 把同一個 1000 訊息立即送出：一樣走官方 CoreManager 程式碼，
            // 只是把窗口從 2 秒縮到數毫秒。欄位不存在（非 308）時維持原排程。
            try {
                Field handlerField = managerClass.getDeclaredField("k");
                Object handler = handlerField.get(null);
                if (handler instanceof Handler) {
                    ((Handler) handler).removeMessages(1000);
                    ((Handler) handler).sendEmptyMessage(1000);
                    Log.i(TAG, "CoreAlive register message fired immediately");
                }
            } catch (Throwable t) {
                Log.w(TAG, "immediate fire unavailable, keep 2s schedule", t);
            }
        } catch (Throwable t) {
            Log.e(TAG, "CoreAlive registration failed", t);
        }
    }

    /** 保底綁定用的連線回呼；刻意不 unbind，进程死亡時由 binder death 清理。 */
    private static final class BootBindConnection implements ServiceConnection {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            Log.i(TAG, "VoiceTriggerService connected: " + name);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            Log.w(TAG, "VoiceTriggerService disconnected: " + name);
        }
    }
}
