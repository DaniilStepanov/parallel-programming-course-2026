package lab2;

import java.util.List;

/** Decides who runs next. Multi-run strategies (DFS) also say whether another run is needed. */
public interface Strategy {
    int pick(List<Integer> enabled, int last);
    default void beginRun() { }
    default boolean endRun() { return false; }   // true = run again
}
