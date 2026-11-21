package lab3;

import java.util.concurrent.CompletableFuture;

/**
 * What you write in stage 2. The harness calls {@link #search} at a fixed rate from one thread and does not
 * wait for the result before the next call, so {@code search} must return quickly; complete the future when
 * the answer is ready, at the latest at the search deadline.
 *
 * <p>An implementation must have a public constructor taking {@link Kit}.
 */
public interface Aggregator extends AutoCloseable {
    CompletableFuture<Answer> search(Search s);

    /** Called once after the run; stop your threads here. */
    @Override default void close() throws Exception {}
}
