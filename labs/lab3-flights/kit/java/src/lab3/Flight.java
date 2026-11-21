package lab3;

/** One flight from schedule.bin. Times are minutes from 2026-12-01 00:00; basePrice is the schedule price. */
public record Flight(int id, String airline, String from, String to, int dep, int arr, int basePrice) {
    /** Day of departure: the day the airline uses for this segment. */
    public int day() { return dep / 1440; }

    /** The airline request that returns the live price of this flight (and of all flights of the airline on this segment and day). */
    public PriceKey priceKey() { return new PriceKey(airline, from, to, day()); }
}
