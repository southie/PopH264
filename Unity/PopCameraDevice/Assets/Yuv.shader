﻿Shader "Unlit/Yuv"
{
	Properties
	{
		_MainTex ("Texture", 2D) = "white" {}
		ChromaUTexture ("ChromaU", 2D) = "black" {}
		ChromaVTexture ("ChromaV", 2D) = "black" {}
		LumaMin("LumaMin", Range(0,255) ) = 16
		LumaMax("LumaMax", Range(0,255) ) = 253
		ChromaVRed("ChromaVRed", Range(-2,2) ) = 1.5958
		ChromaUGreen("ChromaUGreen", Range(-2,2) ) = -0.39173
		ChromaVGreen("ChromaVGreen", Range(-2,2) ) = -0.81290
		ChromaUBlue("ChromaUBlue", Range(-2,2) ) = 2.017
		[Toggle]Flip("Flip", Range(0,1)) = 1
		[Toggle]EnableChroma("EnableChroma", Range(0,1)) = 1
	}
	SubShader
	{
		Tags { "RenderType"="Opaque" }
		LOD 100

		Pass
		{
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			
			#include "UnityCG.cginc"

			struct appdata
			{
				float4 vertex : POSITION;
				float2 uv : TEXCOORD0;
			};

			struct v2f
			{
				float2 uv : TEXCOORD0;
				float4 vertex : SV_POSITION;
			};

			sampler2D _MainTex;
			float4 _MainTex_ST;
		#define LumaTexture _MainTex
			sampler2D ChromaUTexture;
			sampler2D ChromaVTexture;

			float LumaMin;
			float LumaMax;
			float ChromaVRed;
			float ChromaUGreen;
			float ChromaVGreen;
			float ChromaUBlue;


			float Flip;
			float EnableChroma;
			#define FLIP	( Flip > 0.5f )	
			#define ENABLE_CHROMA	( EnableChroma > 0.5f )
			
			v2f vert (appdata v)
			{
				v2f o;
				o.vertex = UnityObjectToClipPos(v.vertex);
				o.uv = TRANSFORM_TEX(v.uv, _MainTex);

				if ( FLIP )
					o.uv.y = 1 - o.uv.y;

				return o;
			}
			
			fixed4 frag (v2f i) : SV_Target
			{
				// sample the texture
				float Luma = tex2D(LumaTexture, i.uv);
				float ChromaU = tex2D(ChromaUTexture, i.uv);
				float ChromaV = tex2D(ChromaVTexture, i.uv);
				
				ChromaU = lerp(-0.5, 0.5, ChromaU);
				ChromaV = lerp(-0.5, 0.5, ChromaV);
				Luma = lerp(LumaMin/255, LumaMax/255, Luma);

				if ( !ENABLE_CHROMA )
				{
					ChromaU = 0;
					ChromaV = 0;
				}

				float3 Rgb;
				Rgb.x = Luma + (ChromaVRed * ChromaV);
				Rgb.y = Luma + (ChromaUGreen * ChromaU) + (ChromaVGreen * ChromaV);
				Rgb.z = Luma + (ChromaUBlue * ChromaU);

				return float4( Rgb, 1);
			}
			ENDCG
		}
	}
}
