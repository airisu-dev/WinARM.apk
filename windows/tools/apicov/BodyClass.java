// BodyClass -- what a method's code does, read from the start of its bytecode:
// whether it does any work, or is one of the shapes a stub takes.
//
//   REAL      anything else: the method does work (whether the right work is not claimed)
//   EMPTY     returns at once (void), or a constructor that only calls super() with no arguments
//   DEFAULT   returns 0, false, 0.0 or null and nothing else
//   THROWS    throws a new UnsupportedOperationException or RuntimeException, with at most a message
//   NATIVE    native: what it does depends on how the runtime binds it
//   ABSTRACT  abstract: no code of its own
//
// A trivial body is not a stub by itself: many Android callbacks are empty on
// purpose. Bodies compares the shape with AOSP's own method to tell the two
// apart.

package com.winapk.apicov;

import jdk.internal.org.objectweb.asm.Handle;
import jdk.internal.org.objectweb.asm.Label;
import jdk.internal.org.objectweb.asm.MethodVisitor;
import jdk.internal.org.objectweb.asm.Opcodes;

import java.util.function.Consumer;

enum BodyClass {
    REAL, EMPTY, DEFAULT, THROWS, NATIVE, ABSTRACT;

    boolean trivial() { return this == EMPTY || this == DEFAULT || this == THROWS; }

    String id() { return name().toLowerCase(java.util.Locale.ROOT); }

    static BodyClass of(int access) {
        if ((access & Opcodes.ACC_ABSTRACT) != 0) return ABSTRACT;
        if ((access & Opcodes.ACC_NATIVE) != 0) return NATIVE;
        return null;   // read the code
    }

    // A visitor that reads a method's code and hands its class to `done` at the end. Only the first few
    // instructions matter: every trivial shape is at most six long.
    static MethodVisitor reader(String name, Consumer<BodyClass> done) {
        return new MethodVisitor(Opcodes.ASM9) {
            static final int KEEP = 8;
            final int[] ops = new int[KEEP];
            int count;
            String newType, invokeDesc;

            private void op(int opcode) {
                if (count < KEEP) ops[count] = opcode;
                count++;
            }

            @Override public void visitInsn(int opcode) { op(opcode); }
            @Override public void visitIntInsn(int opcode, int operand) { op(opcode); }
            @Override public void visitVarInsn(int opcode, int var) { op(opcode); }
            @Override public void visitFieldInsn(int opcode, String owner, String n, String d) { op(opcode); }
            @Override public void visitJumpInsn(int opcode, Label label) { op(opcode); }
            @Override public void visitLdcInsn(Object value) { op(Opcodes.LDC); }
            @Override public void visitIincInsn(int var, int increment) { op(Opcodes.IINC); }
            @Override public void visitMultiANewArrayInsn(String d, int dims) { op(Opcodes.MULTIANEWARRAY); }

            @Override
            public void visitTableSwitchInsn(int min, int max, Label dflt, Label... labels) {
                op(Opcodes.TABLESWITCH);
            }

            @Override
            public void visitLookupSwitchInsn(Label dflt, int[] keys, Label[] labels) { op(Opcodes.LOOKUPSWITCH); }

            @Override
            public void visitInvokeDynamicInsn(String n, String d, Handle bsm, Object... args) {
                op(Opcodes.INVOKEDYNAMIC);
            }

            @Override
            public void visitTypeInsn(int opcode, String type) {
                if (opcode == Opcodes.NEW && newType == null) newType = type;
                op(opcode);
            }

            @Override
            public void visitMethodInsn(int opcode, String owner, String n, String d, boolean itf) {
                if (opcode == Opcodes.INVOKESPECIAL && invokeDesc == null) invokeDesc = d;
                op(opcode);
            }

            @Override
            public void visitEnd() { done.accept(classify(name, ops, count, newType, invokeDesc)); }
        };
    }

    static BodyClass classify(String name, int[] ops, int count, String newType, String firstSpecialDesc) {
        if (count == 1 && ops[0] == Opcodes.RETURN) return EMPTY;
        // super(): aload_0, invokespecial <init>()V, return.
        if (name.equals("<init>") && count == 3 && ops[0] == Opcodes.ALOAD && ops[1] == Opcodes.INVOKESPECIAL
                && ops[2] == Opcodes.RETURN && "()V".equals(firstSpecialDesc)) {
            return EMPTY;
        }
        if (count == 2 && isZero(ops[0]) && ops[1] >= Opcodes.IRETURN && ops[1] <= Opcodes.ARETURN) return DEFAULT;
        // new X; dup; [ldc "message";] invokespecial <init>; athrow.
        if (count >= 4 && count <= 5 && ops[0] == Opcodes.NEW && ops[1] == Opcodes.DUP
                && ops[count - 1] == Opcodes.ATHROW && ops[count - 2] == Opcodes.INVOKESPECIAL
                && (count == 4 || ops[2] == Opcodes.LDC)
                && ("java/lang/UnsupportedOperationException".equals(newType)
                    || "java/lang/RuntimeException".equals(newType))) {
            return THROWS;
        }
        return REAL;
    }

    private static boolean isZero(int op) {
        return op == Opcodes.ACONST_NULL || op == Opcodes.ICONST_0 || op == Opcodes.LCONST_0
                || op == Opcodes.FCONST_0 || op == Opcodes.DCONST_0;
    }
}
