package lab3;

import java.time.Duration;
import java.util.List;

/**
 * One search: price the candidate routes and answer before {@code deadlineNanos} (System.nanoTime() scale).
 * The deadline is 300 ms after the planned start of the search.
 *
 * @param id      search id, send it with every airline request of this search
 * @param routes  candidate routes; the answer refers to them by index in this list
 */
public record Search(long id, String from, String to, int day, List<Route> routes, long deadlineNanos) {

    /** A candidate route: 1–3 flights. */
    public record Route(List<Flight> flights) {
        public int durationMinutes() { return flights.get(flights.size() - 1).arr() - flights.get(0).dep(); }
    }

    public long remainingNanos() { return deadlineNanos - System.nanoTime(); }

    /** Time left until the deadline, never negative. */
    public Duration remaining() { return Duration.ofNanos(Math.max(0, remainingNanos())); }
}
