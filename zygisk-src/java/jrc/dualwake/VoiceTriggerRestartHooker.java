package jrc.dualwake;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.SystemClock;
import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Makes VoiceTrigger's SoundTrigger restart path idempotent.
 *
 * HyperOS 3 VoiceTrigger (com.miui.voicetrigger, 308) may deliver
 * onResourcesAvailable twice for the same active model. The first callback
 * restarts recognition successfully; the second immediately calls
 * restartRecognition again and returns -38 (INVALID_OPERATION). Keep the first
 * real restart, debounce only the duplicate callback on the same session, and
 * report an already-active model as success.
 *
 * Hook target (308, versionCode 2026051416):
 *   com.miui.voicetrigger.wakeup.q.k(Ljava/lang/String;)I  — restartRecognition
 *
 * LSPlant 會在目標方法被呼叫時改走 callback(Object[])，args[0] 是 this，
 * args[1] 是原本的字串參數；backup 由 native 在 Hook() 成功後填入。
 */
public final class VoiceTriggerRestartHooker {
    private static final String TAG = "DualWake-VT";
    private static final long EXPECTED_VERSION_CODE = 2026051416L;
    private static final long DUPLICATE_WINDOW_MS = 750L;
    private static final int STATUS_INVALID_OPERATION = -38;

    private static final Map<Object, Long> lastSuccess = new WeakHashMap<>();

    /** Filled by native code with the LSPlant backup method after Hook(). */
    public Method backup;

    /**
     * Version + target guard, evaluated before any ART hook is attempted.
     * 只在完全吻合 308 目標時回傳 true；其餘情況安全停用。
     */
    public static boolean shouldInstall(Context context) {
        try {
            PackageInfo info = context.getPackageManager()
                    .getPackageInfo("com.miui.voicetrigger", 0);
            long versionCode = info.getLongVersionCode();
            if (versionCode != EXPECTED_VERSION_CODE) {
                Log.w(TAG, "skip: VoiceTrigger versionCode " + versionCode
                        + " != " + EXPECTED_VERSION_CODE);
                return false;
            }
            Class<?> target = context.getClassLoader()
                    .loadClass("com.miui.voicetrigger.wakeup.q");
            target.getDeclaredMethod("k", String.class);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "skip: restart target unavailable", t);
            return false;
        }
    }

    /** LSPlant callback. Signature fixed as Object callback(Object[] args). */
    public Object callback(Object[] args) throws Throwable {
        Object thiz = args[0];
        long now = SystemClock.elapsedRealtime();

        Long last;
        synchronized (lastSuccess) {
            last = lastSuccess.get(thiz);
        }
        if (last != null && now - last >= 0 && now - last < DUPLICATE_WINDOW_MS) {
            // 同一 SoundTrigger session 剛 restart 成功；直接回報成功，
            // 等價於已完成的操作，避免第二次 HAL 呼叫。
            Log.i(TAG, "debounced duplicate SoundTrigger restart");
            return 0;
        }

        int result;
        try {
            result = (Integer) backup.invoke(thiz, args[1]);
        } catch (InvocationTargetException e) {
            // 原方法自己丟例外：原樣往外拋，語意與未 hook 相同。
            Throwable cause = e.getCause();
            if (cause != null) throw cause;
            throw e;
        }

        if (result == STATUS_INVALID_OPERATION) {
            // VoiceTrigger 走到這裡表示它自己的狀態檢查認為已 STARTED；
            // HAL 回報 recognition 已在進行，視為成功。
            Log.i(TAG, "SoundTrigger model already active; treating restart as success");
            result = 0;
        }
        if (result == 0) {
            synchronized (lastSuccess) {
                lastSuccess.put(thiz, now);
            }
        }
        return result;
    }
}
