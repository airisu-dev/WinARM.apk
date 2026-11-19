package fb;

// Body inherits a real method from here; in the AOSP copy Body declares it itself.
public class BodyBase {
    protected int count;

    public void inheritedReal() { count++; }
}
