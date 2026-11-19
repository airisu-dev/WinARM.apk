package fx;

public interface Iface {
    int IC = 3;

    default void dflt() {}

    static int sm() { return 0; }
}
