// IntersectionScenario.ned - Network definition for WAVE RAFT simulation
// Extends the standard Veins Scenario network

package raftwave;

import org.car2x.veins.nodes.Scenario;
import org.car2x.veins.base.connectionManager.ConnectionManager;
import org.car2x.veins.base.modules.BaseWorldUtility;
import org.car2x.veins.visualizer.roads.RoadsCanvasVisualizer;

// Simple network that extends the standard Veins Scenario
// All configuration is done through omnetpp.ini
network IntersectionScenario extends Scenario
{
    parameters:
        playgroundSizeX = 2500m;
        playgroundSizeY = 2500m;
        playgroundSizeZ = 50m;
        
    submodules:
        connectionManager: ConnectionManager {
            parameters:
                sendDirect = true;
                maxInterfDist = 2600m;
                drawMaxIntfDist = false;
                @display("p=150,0;i=abstract/multicast");
        }
        world: BaseWorldUtility {
            parameters:
                playgroundSizeX = 2500m;
                playgroundSizeY = 2500m;
                playgroundSizeZ = 50m;
                useTorus = false;
                use2D = true;
                @display("p=30,0;i=misc/globe");
        }
        roadsCanvasVisualizer: RoadsCanvasVisualizer {
            @display("p=300,0");
        }
}
