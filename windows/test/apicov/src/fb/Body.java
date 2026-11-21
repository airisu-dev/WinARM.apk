package fb;

public class Body extends BodyBase {
    public void emptyBoth() {}

    public void emptyStub() {}                  // AOSP's does work

    public native int natImpl();                // natives.txt: implemented

    public native int natStub();                // natives.txt: bound to a stub

    public native int natUnbound();             // not in natives.txt: UnsatisfiedLinkError

    public void notInAosp() {}                  // the AOSP copy lacks it

    public Object nullBoth() { return null; }

    public void realHere() { count += 2; }

    public int stubInt() { return 0; }          // AOSP's returns a field

    public void throwsBoth() { throw new UnsupportedOperationException(); }

    public String throwsStub() { throw new UnsupportedOperationException("not on Windows"); }
}
