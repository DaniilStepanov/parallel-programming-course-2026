package lab3;

/**
 * Airline answer.
 * <ul>
 *   <li>{@code OK}: live prices of the airline's flights on the segment that still have seats.
 *       A flight of this segment missing from the list has no seats.</li>
 *   <li>{@code ERR}: temporary error, the request may be repeated.</li>
 *   <li>{@code LIMIT}: the airline already had its limit of unanswered requests; this one was refused.</li>
 *   <li>{@code TIMEOUT}: only from {@link AirlineClient#get}: no answer within the timeout, the request was cancelled.</li>
 * </ul>
 */
public final class PriceResult {
    public enum Kind { OK, ERR, LIMIT, TIMEOUT }

    public static final PriceResult ERR = new PriceResult(Kind.ERR, new int[0], new int[0]);
    public static final PriceResult LIMIT = new PriceResult(Kind.LIMIT, new int[0], new int[0]);
    public static final PriceResult TIMEOUT = new PriceResult(Kind.TIMEOUT, new int[0], new int[0]);

    private final Kind kind;
    private final int[] flightIds, prices;

    PriceResult(Kind kind, int[] flightIds, int[] prices) {
        this.kind = kind;
        this.flightIds = flightIds;
        this.prices = prices;
    }

    public Kind kind() { return kind; }
    public boolean ok() { return kind == Kind.OK; }
    public int size() { return flightIds.length; }
    public int flightId(int i) { return flightIds[i]; }
    public int price(int i) { return prices[i]; }

    /** @return live price of the flight, or -1 if it is not in the answer (no seats). Only meaningful for OK. */
    public int priceOf(int flightId) {
        for (int i = 0; i < flightIds.length; i++) if (flightIds[i] == flightId) return prices[i];
        return -1;
    }

    @Override public String toString() { return kind == Kind.OK ? "OK(" + flightIds.length + " flights)" : kind.name(); }
}
