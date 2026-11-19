package fb;

public interface Cb {
    default void onDefault() {}

    void onThing();
}
