package lab3;

/** What one airline request asks: all flights of {@code airline} from {@code from} to {@code to} departing on {@code day}. */
public record PriceKey(String airline, String from, String to, int day) {
    @Override public String toString() { return airline + ":" + from + ":" + to + ":" + day; }
}
