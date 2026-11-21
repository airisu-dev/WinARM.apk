package fb;

public class Body {
    private int count;

    public void emptyBoth() {}

    public void emptyStub() { count++; }

    public void inheritedReal() { count++; }

    public int natImpl() { return count; }

    public int natStub() { return count; }

    public int natUnbound() { return count; }

    public Object nullBoth() { return null; }

    public void realHere() { count += 2; }

    public int stubInt() { return count; }

    public void throwsBoth() { throw new UnsupportedOperationException(); }

    public String throwsStub() { return String.valueOf(count); }
}
