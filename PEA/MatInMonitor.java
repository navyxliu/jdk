class MatInMonitor {
    private Object _cache;
    public void test() {
        Object x = new Object();
        synchronized (x) {
            _cache = x;
        }
    }

    // c2 doesn't support this nesting form. GeneratePairingInfo regards it as 'unbalanced'.
    public void testNested() {
        Object x = new Object();
        synchronized (x) {
        synchronized (x) {
        synchronized (x) {
            _cache = x;
        }}}
    }

    synchronized void bar() { }
    synchronized void foo() { bar(); }

    // Perfectly fine in C2. it is also a nesting lock after inlining.
    // it passes GeneratePairingInfo becasue it is intra-procedural.
    public void testNested2() {
        synchronized(this) {
            foo();
        }
    }

    public static void main(String[] args)  {
        MatInMonitor kase = new MatInMonitor();

        for (int i = 0; i < 2; ++i) {
            kase.test();
            kase.testNested();
            kase.testNested2();
        }
    }
}
