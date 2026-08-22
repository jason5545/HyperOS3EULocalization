package android.content;

/** Host-side stub: records package + class name. */
public class ComponentName {
    private final String pkg;
    private final String cls;

    public ComponentName(String pkg, String cls) {
        this.pkg = pkg;
        this.cls = cls;
    }

    public String getPackageName() { return pkg; }

    public String getClassName() { return cls; }

    @Override
    public String toString() { return pkg + "/" + cls; }
}
