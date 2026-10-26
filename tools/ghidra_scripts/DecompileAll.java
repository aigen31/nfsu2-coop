// Ghidra post-script: decompile every function to one .c file plus a TSV index.
// Usage: analyzeHeadless <proj> <name> -import <exe> -scriptPath tools/ghidra_scripts \
//          -postScript DecompileAll.java
// Set DECOMP_OUT to the output directory (default: the script directory).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = System.getenv("DECOMP_OUT");
        if (outDir == null || outDir.isEmpty()) outDir = ".";
        File dir = new File(outDir);
        dir.mkdirs();

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();
        PrintWriter all = new PrintWriter(new FileWriter(new File(dir, "all.c")));
        PrintWriter idx = new PrintWriter(new FileWriter(new File(dir, "index.tsv")));
        int count = 0, failed = 0;
        for (Function f : fm.getFunctions(true)) {
            if (monitor.isCancelled()) break;
            DecompileResults res = ifc.decompileFunction(f, 60, monitor);
            String c;
            if (res.getDecompiledFunction() != null) {
                c = res.getDecompiledFunction().getC();
            } else {
                c = "// decompile failed";
                failed++;
            }
            all.println("// " + f.getName() + " @ " + f.getEntryPoint());
            all.println(c);
            idx.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + f.getBody().getNumAddresses());
            count++;
            if ((count % 200) == 0) println("decompiled " + count);
        }
        all.close();
        idx.close();
        ifc.dispose();
        println("DecompileAll: " + count + " functions, " + failed + " failed -> " + dir.getAbsolutePath());
    }
}
