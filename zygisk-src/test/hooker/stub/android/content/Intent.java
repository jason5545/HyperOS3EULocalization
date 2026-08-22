package android.content;

import android.net.Uri;

import java.util.HashMap;
import java.util.Map;

/** Host-side stub: records action/package/data/component/flags/extras, chainable setters. */
public class Intent {
    public final String action;
    public String pkg;
    public Uri data;
    public ComponentName component;
    public int flags;
    public final Map<String, Integer> extrasInt = new HashMap<>();

    public Intent(String action) { this.action = action; }

    public Intent setPackage(String pkg) { this.pkg = pkg; return this; }

    public Intent setData(Uri data) { this.data = data; return this; }

    public Intent setComponent(ComponentName component) { this.component = component; return this; }

    public Intent addFlags(int flags) { this.flags |= flags; return this; }

    public Intent putExtra(String name, int value) { extrasInt.put(name, value); return this; }
}
