class MatInMonitor {
    private Object _cache;
    public void test() {
        Object x = new Object();
        synchronized (x) {
            _cache = x;
        }
    }

    public void test2() {
        Object x = new Object();
        Object y = new Object();
        synchronized (x) {
        synchronized (y) {
            _cache = x;
        }}
    }

    public void test3(boolean cond) {
        Object x = new Object();
        synchronized (x) {
            if (cond) {
                _cache = x;
            }
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

    synchronized void bar() {
        _cache = this; // escaping
    }
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

        for (int i = 0; i < 1_000_000; ++i) {
            kase.test();
            kase.test2();
            kase.test3(0 == (i& 0xf));
            kase.testNested();
            kase.testNested2();
        }
    }
}
