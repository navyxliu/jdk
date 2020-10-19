/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * @test
 * @library /test/lib
 * @bug 8255033
 * @summary enable -XX:+VerifyLoopOptimizations for a simple case
 * @requires vm.debug == true & vm.flavor == "server"
 *
 * @run main/othervm -XX:-TieredCompilation -XX:CompileOnly=java.lang.String::charAt -XX:CompileOnly=java.lang.String::isLatin1 -XX:CompileOnly=java.lang.StingLatin1.charAt -XX:+VerifyLoopOptimizations compiler.loopopts.TestVerifyLoopOpts
 */

package compiler.loopopts;
import java.util.Random;
import jdk.test.lib.Asserts;

public class TestVerifyLoopOpts {
    public static void main(String[] args) {
        test_string_charAt();
    }

    public static void test_string_charAt() {
        char[] letters = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
        Random rand = new Random();
        String str =  new String(letters);

        for (int i = 0; i < 300_000; i++) {
            int j = rand.nextInt(letters.length);
            Asserts.assertEquals(str.charAt(j), letters[j], "wrong results");
        }
    }
}
