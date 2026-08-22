package android.content;

import android.net.Uri;

/** Host-side stub: records action/package/data, chainable setters. */
public class Intent {
    public final String action;
    public String pkg;
    public Uri data;

    public Intent(String action) { this.action = action; }

    public Intent setPackage(String pkg) { this.pkg = pkg; return this; }

    public Intent setData(Uri data) { this.data = data; return this; }
}
