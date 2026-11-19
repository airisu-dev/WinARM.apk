package fx;

public class Outer<T extends Number> {
    public class Inner {
        public Inner(int x) {}

        public T get() { return null; }
    }

    public static class Nested {
        public Nested(int x) {}
    }
}
