package android.os;

import java.util.HashMap;
import java.util.Map;

/** Host-side stub: a plain map. */
public class Bundle {
    public final Map<String, Object> map = new HashMap<>();

    public int getInt(String key, int def) {
        Object v = map.get(key);
        return v instanceof Integer ? (Integer) v : def;
    }

    public void putInt(String key, int value) { map.put(key, value); }
}
