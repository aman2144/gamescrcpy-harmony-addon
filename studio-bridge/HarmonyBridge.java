// GameScrcpy adapter for the mirroring component of a locally installed DevEco Studio.
import com.huawei.hosscrcpy.api.*;
import java.io.*;
import java.net.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public final class HarmonyBridge {
    private final HosRemoteDevice device;
    private final String token;
    private final ServerSocket server;
    private final ExecutorService clients = Executors.newCachedThreadPool();
    private final AtomicBoolean viewing = new AtomicBoolean(), running = new AtomicBoolean(true), ready = new AtomicBoolean();
    private final AtomicLong packets = new AtomicLong();

    private final AtomicLong audioFrames = new AtomicLong();
    private final AtomicInteger audioStreams = new AtomicInteger();
    private volatile int displayWidth, displayHeight;
    private final AtomicBoolean controlling = new AtomicBoolean();
    private final java.util.Set<Socket> sockets = ConcurrentHashMap.newKeySet();
    private volatile Socket videoSocket;
    private boolean held;
    private int lastX, lastY;

    private HarmonyBridge(String serial, String hdc, int port, String token) throws Exception {
        this.token = token;
        com.alibaba.fastjson.JSON.toJSONString(java.util.Map.of("ready", true));
        HosRemoteConfig config = new HosRemoteConfig(serial);
        config.setHdcPath(hdc); config.setScale(1); config.setFrameRate(30);
        config.setBitRate(8); config.setPort(5281); config.setIFrameInterval(1000);
        device = new HosRemoteDevice(config);
        server = new ServerSocket();
        server.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), port));
    }

    private static String line(InputStream in, int limit) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        for (int i = 0; i < limit; i++) {
            int c = in.read();
            if (c < 0) return bytes.size() == 0 ? null : bytes.toString(StandardCharsets.UTF_8);
            if (c == '\n') return bytes.toString(StandardCharsets.UTF_8);
            if (c != '\r') bytes.write(c);
        }
        throw new IOException("Command line too long");
    }

    private void serve(Socket socket) {
        try (socket) {
            socket.setTcpNoDelay(true); socket.setSoTimeout(5000);
            InputStream in = socket.getInputStream();
            String hello = line(in, 256);
            if (hello == null) return;
            String[] fields = hello.split(" ");
            if (fields.length != 3 || !fields[0].equals("GSH1") ||
                !java.security.MessageDigest.isEqual(fields[2].getBytes(StandardCharsets.UTF_8), token.getBytes(StandardCharsets.UTF_8))) return;
            switch (fields[1]) {
                case "VIDEO" -> video(socket);
                case "CONTROL" -> control(socket, in);
                case "AUDIO" -> audio(socket);
                case "STATUS" -> socket.getOutputStream().write(("{\"ready\":" + ready.get() +
                    ",\"packets\":" + packets.get() + ",\"maxContacts\":1,\"audioFrames\":" + audioFrames.get() +
                    ",\"audioActive\":" + (audioStreams.get() > 0) + ",\"width\":" + displayWidth + ",\"height\":" + displayHeight + "}\n").getBytes(StandardCharsets.UTF_8));
                case "STOP" -> close();
                default -> throw new IOException("Unsupported channel");
            }
        } catch (Throwable error) {
            if (running.get()) { System.err.println("CHANNEL_END " + error.getClass().getSimpleName() + ": " + error.getMessage()); error.printStackTrace(); }
        } finally { sockets.remove(socket); }
    }

    private void audio(Socket destination) throws Exception {
        String audioPort = System.getenv("GS_HARMONY_AUDIO_PORT");
        if (audioPort == null) throw new IOException("Signed audio companion is not configured");
        int port = Integer.parseInt(audioPort);
        if (port < 1024 || port > 65535) throw new IOException("Invalid audio forwarding port");
        try (Socket source = new Socket()) {
            sockets.add(source);
            source.connect(new InetSocketAddress("127.0.0.1", port), 3000);
            source.setSoTimeout(10000); source.setTcpNoDelay(true);
            source.getOutputStream().write(("GSA1 " + token + "\n").getBytes(StandardCharsets.US_ASCII));
            DataInputStream in = new DataInputStream(source.getInputStream());
            String format = line(in, 64);
            if (!"GSA1 48000 2 S16LE".equals(format)) throw new IOException("Invalid audio stream format");
            DataOutputStream out = new DataOutputStream(destination.getOutputStream());
            out.write((format + "\n").getBytes(StandardCharsets.US_ASCII));
            audioStreams.incrementAndGet();
            try {
            while (running.get() && !destination.isClosed()) {
                int length = in.readInt();
                if (length < 4 || length > 192000 || length % 4 != 0) throw new IOException("Invalid PCM block");
                byte[] pcm = in.readNBytes(length);
                if (pcm.length != length) throw new EOFException("Truncated PCM block");
                out.writeInt(length); out.write(pcm); out.flush();
                audioFrames.addAndGet(length / 4);
            }
            } finally { audioStreams.decrementAndGet(); }
        } finally {
            sockets.removeIf(Socket::isClosed);
        }
    }

    private void video(Socket socket) throws Exception {
        if (!viewing.compareAndSet(false, true)) throw new IOException("Only one mirror is supported");
        videoSocket = socket;
        ArrayBlockingQueue<byte[]> queue = new ArrayBlockingQueue<>(96);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicBoolean firstPacket = new AtomicBoolean();
        ScheduledExecutorService deadline = Executors.newSingleThreadScheduledExecutor();
        var timeout = deadline.schedule(() -> {
            if (!firstPacket.get()) {
                failure.set(new IOException("No first video packet within 35 seconds"));
                try { socket.close(); } catch (IOException ignored) { }
                device.stopCaptureScreen();
            }
        }, 35, TimeUnit.SECONDS);
        try {
            device.startCaptureScreen(new ScreenCapCallback() {
                public void onReady() { ready.set(true); device.requestIDRFrame(); System.out.println("VIDEO_READY"); }
                public void onData(ByteBuffer source) {
                    ByteBuffer copy = source.duplicate();
                    if (copy.remaining() < 1 || copy.remaining() > 8 * 1024 * 1024) return;
                    byte[] bytes = new byte[copy.remaining()]; copy.get(bytes);
                    firstPacket.set(true); packets.incrementAndGet(); timeout.cancel(false);
                    if (!queue.offer(bytes)) {
                        failure.set(new IOException("Video consumer fell behind"));
                        try { socket.close(); } catch (IOException ignored) { }
                    }
                }
                public void onException(Throwable error) {
                    failure.set(error);
                    try { socket.close(); } catch (IOException ignored) { }
                }
            });
            DataOutputStream out = new DataOutputStream(new BufferedOutputStream(socket.getOutputStream(), 256 * 1024));
            while (running.get() && !socket.isClosed() && failure.get() == null) {
                byte[] data = queue.poll(1, TimeUnit.SECONDS);
                out.writeInt(data == null ? 0 : data.length);
                if (data != null) out.write(data);
                out.flush();
            }
            if (failure.get() != null) throw new IOException("Capture failed", failure.get());
        } finally {
            deadline.shutdownNow(); ready.set(false); release(); device.stopCaptureScreen();
            viewing.set(false); videoSocket = null;
            System.out.println("VIDEO_STOPPED packets=" + packets.get());
        }
    }

    private synchronized void release() {
        if (held) { held = false; device.onTouchUp(lastX, lastY); }
    }

    private synchronized boolean touch(int action, int x, int y) {
        if (!ready.get() || x < 0 || y < 0 || x > 4095 || y > 4095) return false;
        if (action == 0 && !held) { device.onTouchDown(x, y); held = true; }
        else if (action == 1 && held) device.onTouchMove(x, y);
        else if (action == 2 || action == 3) { if (held) device.onTouchUp(x, y); held = false; }
        else return false;
        lastX = x; lastY = y;
        return true;
    }

    private void control(Socket socket, InputStream in) throws Exception {
        if (!controlling.compareAndSet(false, true)) throw new IOException("Control is already in use");
        try {
            socket.setSoTimeout(3500);
            for (String command; running.get() && (command = line(in, 8192)) != null;) {
                String[] fields = command.split(" ", 4);
                boolean accepted = false;
                if (fields[0].equals("T") && fields.length == 4) {
                    try { accepted = touch(Integer.parseInt(fields[1]), Integer.parseInt(fields[2]), Integer.parseInt(fields[3])); }
                    catch (NumberFormatException invalid) { accepted = false; }
                }
                else if (command.equals("R")) { release(); accepted = true; }
                else if (command.equals("P")) accepted = true;
                else if (fields[0].equals("D") && fields.length == 3) {
                    try {
                        int w = Integer.parseInt(fields[1]), h = Integer.parseInt(fields[2]);
                        if ((w == 0 && h == 0) || (w >= 2 && h >= 2 && w <= 4096 && h <= 4096 && (long)w * h <= 10000000)) {
                            displayWidth = w; displayHeight = h; accepted = true;
                        }
                    } catch (NumberFormatException invalid) { accepted = false; }
                }
                else if (fields[0].equals("K") && fields.length == 2 && ready.get()) {
                    String key = switch (fields[1]) { case "4" -> "Back"; case "3" -> "Home"; default -> null; };
                    if (key != null) { device.executeShellCommand("uitest uiInput keyEvent " + key, 5); accepted = true; }
                }
                socket.getOutputStream().write((accepted ? "OK\n" : "ER\n").getBytes(StandardCharsets.US_ASCII));
                socket.getOutputStream().flush();
            }
        } finally { try { release(); } finally { controlling.set(false); } }
    }

    private void close() {
        if (!running.compareAndSet(true, false)) return;
        try { server.close(); } catch (IOException ignored) { }
        try { if (videoSocket != null) videoSocket.close(); } catch (IOException ignored) { }
        for (Socket socket : sockets) { try { socket.close(); } catch (IOException ignored) { } }
        release(); device.stopCaptureScreen(); clients.shutdownNow();
    }

    public static void main(String[] args) throws Exception {
        String token = System.getenv("GS_HARMONY_TOKEN");
        if (args.length != 3 || token == null || token.length() < 32) throw new IllegalArgumentException("Missing session arguments");
        HarmonyBridge bridge = new HarmonyBridge(args[0], args[1], Integer.parseInt(args[2]), token);
        Runtime.getRuntime().addShutdownHook(new Thread(bridge::close));
        System.out.println("BRIDGE_READY port=" + bridge.server.getLocalPort() + " maxContacts=1");
        try {
            while (bridge.running.get()) {
                Socket socket = bridge.server.accept();
                if (bridge.sockets.size() >= 8) { socket.close(); continue; }
                bridge.sockets.add(socket);
                bridge.clients.submit(() -> bridge.serve(socket));
            }
        } catch (SocketException closed) {
            if (bridge.running.get()) throw closed;
        } finally { bridge.close(); }
        System.exit(0);
    }
}
