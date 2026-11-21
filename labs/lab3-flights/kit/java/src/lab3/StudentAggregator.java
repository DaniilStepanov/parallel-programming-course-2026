package lab3;

import java.util.concurrent.CompletableFuture;

/** Your stage 2 solution. Run: ./run.sh --impl lab3.StudentAggregator --rate 100 */
public class StudentAggregator implements Aggregator {
    private final Kit kit;

    public StudentAggregator(Kit kit) { this.kit = kit; }

    @Override public CompletableFuture<Answer> search(Search s) {
        throw new UnsupportedOperationException("TODO stage 2");
    }
}
