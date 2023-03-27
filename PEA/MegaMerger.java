class MegaMerger {
  public boolean flag;

  void foo() throws Exception {
    if (flag) { // a global
        throw new Exception("ghost");
    }
  }

  void blackhole(Object obj) throws Exception {}

  void test() {
    Object obj = new Object();
    try {
        foo();
        foo();
        blackhole(obj);  // materialize here
        foo();
    } catch (Exception e) {
/* ingress
Merging state at block #4 bci:28 with empty state on path 2    // starting with 2 because all incoming edges are "new paths".
Merging state at block #4 bci:28 with previous state on path 3
Merging state at block #4 bci:28 with previous state on path 4 - from the 2nd foo()
Merging state at block #4 bci:28 with previous state on path 5 -
Merging state at block #4 bci:28 with previous state on path 6 <-- blackhole
Merging state at block #4 bci:28 with previous state on path 7 - from the 3rd foo()
Merging state at block #4 bci:28 with previous state on path 8 -
*/
        throw new RuntimeException(obj.toString());
    }
  }

  void test2(boolean cond) {
    Object obj = new Object();
    try {
        foo();
        if (cond) {
            blackhole(obj);  // materialize here
        } else {
            foo();
        }
    } catch (Exception e) {
        throw new RuntimeException(obj.toString());
    }
  }


  void simple() {
   Object obj = new Object();
    try {
        foo();
    } catch (Exception e) {
        throw new RuntimeException(obj.toString());
    }
  }

  public static void main(String[] args) {
    MegaMerger kase = new MegaMerger();
    for (int i=0; i < 200_000; ++i) {
        kase.flag = 0 == (i % 1000);
        try {
            kase.simple();
            kase.test();
            kase.test2((i % 5) == 0);
        } catch (RuntimeException e) {
        }
    }
  }
}
