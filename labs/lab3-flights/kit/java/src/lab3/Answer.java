package lab3;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Answer to a search: one result per candidate route (same order as {@link Search#routes()}) and the top.
 *
 * <ul>
 *   <li>PRICED(price): every flight of the route has a live price; price = their sum.</li>
 *   <li>NO_SEATS: an airline answered OK and one of the route's flights is not in its answer.</li>
 *   <li>UNPRICED: no OK answer by the deadline for at least one flight of the route.</li>
 * </ul>
 * Build it with {@link #of}, which also computes the top.
 */
public record Answer(List<RouteResult> routes, List<Integer> top) {
    public enum Status { PRICED, NO_SEATS, UNPRICED }

    public record RouteResult(Status status, int price) {
        public static RouteResult priced(int price) { return new RouteResult(Status.PRICED, price); }
        public static final RouteResult NO_SEATS = new RouteResult(Status.NO_SEATS, 0);
        public static final RouteResult UNPRICED = new RouteResult(Status.UNPRICED, 0);
    }

    public static final int TOP = 10;

    /** Top: indices of PRICED routes by price, then duration, then index; at most {@link #TOP}. */
    public static Answer of(Search s, List<RouteResult> results) {
        if (results.size() != s.routes().size()) throw new IllegalArgumentException("one result per route expected");
        List<Integer> priced = new ArrayList<>();
        for (int i = 0; i < results.size(); i++) if (results.get(i).status() == Status.PRICED) priced.add(i);
        priced.sort(Comparator.<Integer>comparingInt(i -> results.get(i).price())
                .thenComparingInt(i -> s.routes().get(i).durationMinutes())
                .thenComparingInt(i -> i));
        return new Answer(List.copyOf(results), List.copyOf(priced.subList(0, Math.min(TOP, priced.size()))));
    }

    public boolean partial() {
        for (RouteResult r : routes) if (r.status() == Status.UNPRICED) return true;
        return false;
    }
}
