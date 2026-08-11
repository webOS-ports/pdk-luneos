// Decompile every function in the program to C, and emit a per-function
// summary of outgoing calls + referenced strings (the useful bit for
// reimplementing libpdl against luna-service2).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Symbol;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class DecompileAll extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String cPath = a.length > 0 ? a[0] : "/tmp/decomp.c";
        String mPath = a.length > 1 ? a[1] : "/tmp/decomp_map.txt";

        DecompInterface d = new DecompInterface();
        d.setOptions(new DecompileOptions());
        if (!d.openProgram(currentProgram)) {
            println("FATAL: decompiler failed to open program: " + d.getLastMessage());
            return;
        }

        PrintWriter c = new PrintWriter(new FileWriter(cPath));
        PrintWriter m = new PrintWriter(new FileWriter(mPath));

        c.println("/* Decompiled from " + currentProgram.getName() + " by Ghidra headless. */");
        m.println("# function | address | size | -> outgoing calls | strings");

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int total = 0, ok = 0, failed = 0;

        while (it.hasNext()) {
            if (monitor.isCancelled()) break;
            Function f = it.next();
            total++;

            c.println();
            c.println("/* ===== " + f.getName() + " @ " + f.getEntryPoint()
                      + " (" + f.getBody().getNumAddresses() + " bytes) ===== */");

            DecompileResults r = d.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                c.println(r.getDecompiledFunction().getC());
                ok++;
            } else {
                c.println("/* DECOMPILE FAILED: "
                          + (r == null ? "null result" : r.getErrorMessage()) + " */");
                failed++;
            }

            m.println();
            m.println("FUNC " + f.getName() + " @ " + f.getEntryPoint()
                      + " size=" + f.getBody().getNumAddresses());
            for (String s : outgoingCalls(f)) m.println("    -> " + s);
            for (String s : referencedStrings(f)) m.println("    \" " + s);
        }

        c.close();
        m.close();
        d.dispose();
        println("DecompileAll: " + total + " functions, " + ok + " ok, " + failed + " failed");
        println("  C   -> " + cPath);
        println("  map -> " + mPath);
    }

    private Set<String> outgoingCalls(Function f) {
        Set<String> out = new LinkedHashSet<>();
        InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (ii.hasNext()) {
            Instruction ins = ii.next();
            for (Reference ref : ins.getReferencesFrom()) {
                RefType t = ref.getReferenceType();
                if (!t.isCall()) continue;
                Function callee = getFunctionAt(ref.getToAddress());
                if (callee != null) {
                    out.add(callee.getName() + (callee.isThunk() ? " [thunk]" : ""));
                } else {
                    Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(ref.getToAddress());
                    out.add(s != null ? s.getName() : ref.getToAddress().toString());
                }
            }
        }
        return out;
    }

    private List<String> referencedStrings(Function f) {
        List<String> out = new ArrayList<>();
        InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (ii.hasNext()) {
            Instruction ins = ii.next();
            for (Reference ref : ins.getReferencesFrom()) {
                Address to = ref.getToAddress();
                if (to == null) continue;
                Data data = getDataAt(to);
                // follow one level of pointer indirection (literal pools)
                if (data != null && data.isPointer()) {
                    Object v = data.getValue();
                    if (v instanceof Address) data = getDataAt((Address) v);
                }
                if (data == null) continue;
                StringDataInstance sdi = StringDataInstance.getStringDataInstance(data);
                if (sdi == null || sdi == StringDataInstance.NULL_INSTANCE) continue;
                String s = sdi.getStringValue();
                if (s == null) continue;
                s = s.trim();
                if (s.length() >= 4 && !out.contains(s)) out.add(s);
            }
        }
        return out;
    }
}
