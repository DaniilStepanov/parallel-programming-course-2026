package lab3;

/**
 * Live price and seat availability of a flight in one search, as the airline mock reports them.
 * Integer-only, so Java and C++ give the same numbers.
 *
 *   key      = (flightId << 32) ^ searchId              (64-bit, unsigned arithmetic)
 *   price    = base * (800 + (mix(key) >>> 32) % 601) / 1000          base = price from schedule.bin
 *   noSeats  = (mix(key ^ 0x5EA75EA75EA75EA7) >>> 32) % 100 < 10      about 10 % of flights
 *   mix      = splitmix64 finalizer
 */
public final class Pricing {
    private Pricing() {}

    public static long mix(long z) {
        z += 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    static long key(int flightId, long searchId) { return ((long) flightId << 32) ^ searchId; }

    public static int price(int flightId, long searchId, int basePrice) {
        long factor = 800 + Long.remainderUnsigned(mix(key(flightId, searchId)) >>> 32, 601);
        return (int) (basePrice * factor / 1000);
    }

    public static boolean noSeats(int flightId, long searchId) {
        return Long.remainderUnsigned(mix(key(flightId, searchId) ^ 0x5EA75EA75EA75EA7L) >>> 32, 100) < 10;
    }
}
