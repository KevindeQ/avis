struct VS_Output
{
    float4 position : SV_Position;
    float2 texture_coords : TEXCOORD0;
};

VS_Output VS_FullscreenQuad(uint vertex_id : SV_VertexID)
{
    VS_Output output;
    
    output.texture_coords.x = (vertex_id == 2) ? 2.0 : 0.0;
    output.texture_coords.y = (vertex_id == 1) ? 2.0 : 0.0;
 
    output.position = float4(output.texture_coords * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    
    return output;
}