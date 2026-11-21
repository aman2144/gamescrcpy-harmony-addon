import java.net.*;
import java.nio.file.*;
import java.util.*;

class HosLauncher {
    public static void main(String[] args) throws Exception {
        String[] paths = args[0].split(";");
        URL[] urls = new URL[paths.length];
        for (int i = 0; i < paths.length; i++) urls[i] = Path.of(paths[i]).toUri().toURL();
        try (URLClassLoader loader = new URLClassLoader(urls, ClassLoader.getPlatformClassLoader()) {
            public Enumeration<URL> getResources(String name) throws java.io.IOException {
                Enumeration<URL> resources = super.getResources(name);
                if (!name.startsWith("META-INF/services/io.grpc.")) return resources;
                ArrayList<URL> accepted = new ArrayList<>();
                while (resources.hasMoreElements()) {
                    URL url = resources.nextElement();
                    if (!url.toString().contains("hosScrcpy-")) accepted.add(url);
                }
                return Collections.enumeration(accepted);
            }
        }) {
            Thread.currentThread().setContextClassLoader(loader);
            var main = loader.loadClass(args[1]).getMethod("main", String[].class);
            // Resolve gRPC transport providers from the separately supplied dependencies.
            main.invoke(null, (Object) Arrays.copyOfRange(args, 2, args.length));
        }
    }
}
