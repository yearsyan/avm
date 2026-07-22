// SPDX-License-Identifier: MIT

package dev.macmu.agent;

import java.io.BufferedWriter;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class MacMuAgentProtocolTest {
    private MacMuAgentProtocolTest() {}

    public static void main(String[] args) throws Exception {
        String response =
                "1 ok [{\"pkg\":\"com.example.chat\",\"name\":\"微信\"}]\n";
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        BufferedWriter writer = MacMuAgent.createControlResponseWriter(output);
        writer.write(response);
        writer.flush();

        byte[] expected = response.getBytes(StandardCharsets.UTF_8);
        byte[] actual = output.toByteArray();
        if (!Arrays.equals(actual, expected)) {
            throw new AssertionError("control response was not encoded as UTF-8");
        }
        System.out.println("MacMuAgentProtocolTest: PASS");
    }
}
