package lab3;

import java.util.concurrent.CompletableFuture;

/**
 * A request in flight, returned by {@link AirlineClient#getAsync}.
 *
 * <p>{@link #future()} completes on the client's reader thread when the airline answers, so continuations
 * attached without an executor run on that thread.
 *
 * <p><b>Cancellation.</b> Only {@link #cancel()} tells the airline to stop. {@code future().cancel(true)},
 * {@code orTimeout}, {@code completeOnTimeout} and similar only complete the future object; the request stays
 * in flight at the airline, still counts against its limit and is reported as an orphan if it is still
 * unanswered when the search is done.
 */
public interface PriceCall {
    CompletableFuture<PriceResult> future();

    /**
     * Sends CANCEL to the airline and completes {@link #future()} with a CancellationException.
     * @return false if the answer had already arrived (nothing to cancel)
     */
    boolean cancel();
}
